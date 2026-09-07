#!/usr/bin/env python3
"""Merge compiler-link fragments into a deterministic PostgreSQL link database."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import tempfile
from pathlib import Path
from typing import Any

from cc_capture import LINK_FRAGMENT_KIND
from merge_compile_db import inside


DATABASE_KIND = "postgamma.link-command-database"
INPUT_SUFFIXES = {
    ".a": "archive",
    ".lib": "archive",
    ".lo": "object",
    ".o": "object",
    ".obj": "object",
}


class LinkDatabaseError(ValueError):
    """A captured link command is structurally invalid or ambiguous."""


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def _resolve(value: str, directory: Path) -> Path:
    path = Path(value)
    return path.resolve() if path.is_absolute() else (directory / path).resolve()


def classify_target(output: Path, build_root: Path) -> str:
    try:
        relative = output.relative_to(build_root)
    except ValueError:
        return "outside-build"
    parent = relative.parent.as_posix()
    name = relative.name
    if parent == "src/backend" and name in {"postgres", "postgres.exe"}:
        return "postgres-backend"
    if parent == "src/interfaces/libpq" and name in {"libpq.a", "libpq.lib"}:
        return "postgres-libpq-static"
    if parent == "src/interfaces/libpq" and (
        name.startswith("libpq.so")
        or name.startswith("libpq.dylib")
        or name.lower().endswith("pq.dll")
    ):
        return "postgres-libpq-shared"
    if parent == "src/bin/initdb" and name in {"initdb", "initdb.exe"}:
        return "postgres-initdb"
    return "other"


def command_facts(arguments: list[str], directory: Path) -> dict[str, Any]:
    if not arguments or not all(isinstance(argument, str) for argument in arguments):
        raise LinkDatabaseError("link arguments must be a non-empty argv array")
    inputs: list[dict[str, Any]] = []
    library_arguments: list[str] = []
    linker_arguments: list[str] = []
    index = 1
    while index < len(arguments):
        argument = arguments[index]
        if argument == "-o":
            if index + 1 >= len(arguments):
                raise LinkDatabaseError("link command has -o without an output path")
            index += 2
            continue
        if argument.startswith("-o") and len(argument) > 2:
            index += 1
            continue
        if argument in {"-L", "-l"}:
            if index + 1 >= len(arguments):
                raise LinkDatabaseError(f"link command has {argument} without a value")
            library_arguments.extend((argument, arguments[index + 1]))
            index += 2
            continue
        if argument.startswith("-L") or argument.startswith("-l"):
            library_arguments.append(argument)
            index += 1
            continue
        if argument.startswith("@") and len(argument) > 1:
            inputs.append(
                {
                    "argv_index": index,
                    "kind": "response-file",
                    "path": str(_resolve(argument[1:], directory)),
                }
            )
            index += 1
            continue
        suffix_kind = INPUT_SUFFIXES.get(Path(argument).suffix.lower())
        if suffix_kind is not None and not argument.startswith("-"):
            inputs.append(
                {
                    "argv_index": index,
                    "kind": suffix_kind,
                    "path": str(_resolve(argument, directory)),
                }
            )
        else:
            linker_arguments.append(argument)
        index += 1
    return {
        "ordered_inputs": inputs,
        "library_arguments": library_arguments,
        "linker_arguments": linker_arguments,
    }


def normalize_fragment(
    document: dict[str, Any], fragment: Path, build_root: Path
) -> dict[str, Any] | None:
    if document.get("schema_version") != 1 or document.get("kind") != LINK_FRAGMENT_KIND:
        raise LinkDatabaseError(f"{fragment}: invalid link fragment schema")
    allowed = {"schema_version", "kind", "directory", "output", "arguments"}
    unknown = sorted(set(document) - allowed)
    if unknown:
        raise LinkDatabaseError(
            f"{fragment}: unknown field(s): " + ", ".join(unknown)
        )
    directory_value = document.get("directory")
    output_value = document.get("output")
    arguments = document.get("arguments")
    if not isinstance(directory_value, str) or not directory_value:
        raise LinkDatabaseError(f"{fragment}: directory must be a non-empty string")
    if not isinstance(output_value, str) or not output_value:
        raise LinkDatabaseError(f"{fragment}: output must be a non-empty string")
    if not isinstance(arguments, list):
        raise LinkDatabaseError(f"{fragment}: arguments must be an array")
    directory = Path(directory_value).resolve()
    output = _resolve(output_value, directory)
    if not inside(output, build_root):
        return None
    return {
        "target": classify_target(output, build_root),
        "directory": str(directory),
        "output": str(output),
        "arguments": list(arguments),
        **command_facts(list(arguments), directory),
    }


def merge_fragments(
    fragments: Path, build_root: Path, configure_state: Path, config_log: Path
) -> dict[str, Any]:
    records: dict[str, dict[str, Any]] = {}
    for fragment in sorted(fragments.glob("*.json")):
        try:
            document = json.loads(fragment.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError) as exc:
            raise LinkDatabaseError(f"cannot read {fragment}: {exc}") from exc
        if not isinstance(document, dict):
            raise LinkDatabaseError(f"{fragment}: top-level value must be an object")
        normalized = normalize_fragment(document, fragment, build_root)
        if normalized is None:
            continue
        output = normalized["output"]
        previous = records.get(output)
        if previous is not None and previous != normalized:
            raise LinkDatabaseError(
                f"ambiguous link commands captured for output {output}"
            )
        records[output] = normalized
    if not records:
        raise LinkDatabaseError(f"no link commands found beneath {build_root}")
    commands = sorted(
        records.values(),
        key=lambda command: (
            command["target"],
            command["output"],
            command["directory"],
            command["arguments"],
        ),
    )
    return {
        "schema_version": 1,
        "kind": DATABASE_KIND,
        "identity": {
            "configure_state_sha256": sha256(configure_state),
            "toolchain_config_log_sha256": sha256(config_log),
        },
        "build_root": str(build_root),
        "commands": commands,
    }


def write_json(path: Path, document: dict[str, Any]) -> None:
    content = json.dumps(document, indent=2, sort_keys=True) + "\n"
    path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(
        mode="w", encoding="utf-8", dir=path.parent, prefix=".link-db-", delete=False
    ) as handle:
        handle.write(content)
        temporary = Path(handle.name)
    try:
        os.replace(temporary, path)
    finally:
        temporary.unlink(missing_ok=True)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--fragments", required=True, type=Path)
    parser.add_argument("--build-root", required=True, type=Path)
    parser.add_argument("--configure-state", required=True, type=Path)
    parser.add_argument("--config-log", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    try:
        document = merge_fragments(
            args.fragments.resolve(),
            args.build_root.resolve(),
            args.configure_state.resolve(),
            args.config_log.resolve(),
        )
        write_json(args.output.resolve(), document)
    except (LinkDatabaseError, OSError) as exc:
        parser.error(str(exc))
    counts: dict[str, int] = {}
    for command in document["commands"]:
        counts[command["target"]] = counts.get(command["target"], 0) + 1
    summary = ", ".join(f"{target}={counts[target]}" for target in sorted(counts))
    print(f"link database: {len(document['commands'])} commands ({summary}) -> {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
