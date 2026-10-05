"""Read workflow structure and declared shell commands without executing them.

This is a contract check for the repository's workflows, not a Bash evaluator.
Runtime success must still be established by the artifact and execution gates.
"""

from __future__ import annotations

import re
import shlex
import subprocess
from pathlib import Path
from typing import Any


class WorkflowContractError(RuntimeError):
    """A release workflow no longer implements the reviewed contract."""


def require(condition: bool, message: str) -> None:
    if not condition:
        raise WorkflowContractError(message)


def mapping(value: Any, label: str) -> dict[str, Any]:
    require(isinstance(value, dict), f"{label} must be a mapping")
    return value


def read_workflow(path: Path) -> dict[str, Any]:
    try:
        import yaml
    except ImportError as error:
        raise WorkflowContractError(
            "workflow checks require PyYAML; install buildsys/requirements.txt "
            "with the Python interpreter running this check"
        ) from error

    class UniqueLoader(yaml.SafeLoader):
        # YAML 1.1 treats the Actions key 'on' as True. Restrict boolean
        # resolution to true/false without changing PyYAML's global loader.
        yaml_implicit_resolvers = {
            key: [(tag, pattern) for tag, pattern in entries if tag != "tag:yaml.org,2002:bool"]
            for key, entries in yaml.SafeLoader.yaml_implicit_resolvers.items()
        }

        def construct_mapping(self, node, deep=False):
            result = {}
            for key_node, value_node in node.value:
                key = self.construct_object(key_node, deep=deep)
                require(isinstance(key, str), "workflow mapping keys must be strings")
                require(key not in result, f"duplicate workflow key: {key}")
                result[key] = self.construct_object(value_node, deep=deep)
            return result

    UniqueLoader.add_implicit_resolver(
        "tag:yaml.org,2002:bool",
        re.compile(r"^(?:true|false)$", re.IGNORECASE),
        list("tTfF"),
    )
    try:
        document = yaml.load(path.read_text(encoding="utf-8"), Loader=UniqueLoader)
    except yaml.YAMLError as error:
        raise WorkflowContractError(f"invalid workflow YAML: {path}: {error}") from error
    return mapping(document, "workflow")


def argv(command: list[str]) -> list[str]:
    """Unwrap the explicit environment/user wrappers used by these workflows."""

    words = list(command)
    if words and words[0] in ("if", "then"):
        words.pop(0)
    if words[:1] == ["run_as_postgamma"]:
        words.pop(0)
    if words[:1] == ["runuser"] and "--" in words:
        words = words[words.index("--") + 1 :]
    if words[:1] == ["env"]:
        words.pop(0)
    while words and re.match(r"^[A-Za-z_][A-Za-z_0-9]*=", words[0]):
        words.pop(0)
    return words


def shell_commands(script: str) -> list[list[str]]:
    """Recognize simple commands and Bash stdin blocks; ignore data heredocs."""

    syntax = subprocess.run(
        ["bash", "--noprofile", "--norc", "-n"],
        input=script,
        text=True,
        capture_output=True,
        check=False,
    )
    require(syntax.returncode == 0, f"invalid workflow shell: {syntax.stderr.strip()}")

    def read(lines: list[str]) -> list[list[str]]:
        commands = []
        index = 0
        while index < len(lines):
            line = lines[index]
            index += 1
            lexer = shlex.shlex(line, posix=True, punctuation_chars=";&|<>()")
            lexer.whitespace_split = True
            try:
                words = list(lexer)
            except ValueError as error:
                raise WorkflowContractError(f"unsupported workflow shell line: {line}") from error
            if not words:
                continue
            if "<<" in words or "<<-" in words:
                redirect = words.index("<<") if "<<" in words else words.index("<<-")
                require(redirect + 1 < len(words), "missing heredoc delimiter")
                delimiter = words[redirect + 1]
                body = []
                while index < len(lines) and lines[index].lstrip("\t") != delimiter:
                    body.append(lines[index])
                    index += 1
                require(index < len(lines), f"missing heredoc end: {delimiter}")
                index += 1
                words = words[:redirect]
                program = argv(words)
                if program[:1] == ["bash"] or (
                    program[:2] == ["docker", "run"] and "bash" in program[2:]
                ):
                    commands.extend(read(body))
            # Retain array assignments as one declaration (for draft flags).
            if len(words) > 1 and words[0].endswith("=") and words[1] == "(":
                commands.append(words)
                continue
            command = []
            for word in words:
                if word in ("|", "||", "&&", ";", "(", ")"):
                    if command:
                        commands.append(command)
                    command = []
                else:
                    command.append(word)
            if command:
                commands.append(command)
        return commands

    return read(script.replace("\\\n", "").splitlines())


def option(command: list[str], name: str) -> list[str]:
    """Read repeated --name value and --name=value arguments equivalently."""

    values = []
    for index, word in enumerate(command):
        if word.startswith(name + "="):
            values.append(word[len(name) + 1 :])
        elif word == name and index + 1 < len(command):
            values.append(command[index + 1])
    return values
