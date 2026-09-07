#!/usr/bin/env python3
"""Compiler launcher that records atomic compile and link fragments."""

from __future__ import annotations

import json
import os
import shlex
import sys
import tempfile
import uuid
from pathlib import Path


SOURCE_SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".m", ".mm"}
NON_LINK_MODES = frozenset({"-c", "-E", "-M", "-MM", "-S"})
LINK_FRAGMENT_KIND = "postgamma.link-command-fragment"


def compiler_arguments_from_environment(environment: dict[str, str]) -> list[str]:
    """Return wrapper-only arguments without exposing them to the child make."""
    return shlex.split(environment.get("POSTGAMMA_REAL_CC_ARGS", ""))


def source_arguments(arguments: list[str], directory: Path) -> list[Path]:
    if "-c" not in arguments:
        return []
    sources: list[Path] = []
    for value in arguments:
        if value.startswith("-") or Path(value).suffix.lower() not in SOURCE_SUFFIXES:
            continue
        path = Path(value)
        resolved = path.resolve() if path.is_absolute() else (directory / path).resolve()
        if resolved.is_file():
            sources.append(resolved)
    return sources


def output_argument(arguments: list[str], directory: Path) -> Path | None:
    values: list[str] = []
    index = 0
    while index < len(arguments):
        argument = arguments[index]
        if argument == "-o":
            if index + 1 >= len(arguments):
                return None
            index += 1
            values.append(arguments[index])
        elif argument.startswith("-o") and len(argument) > 2:
            values.append(argument[2:])
        index += 1
    if len(values) != 1:
        return None
    output = Path(values[0])
    return output.resolve() if output.is_absolute() else (directory / output).resolve()


def link_output(arguments: list[str], directory: Path) -> Path | None:
    if any(argument in NON_LINK_MODES for argument in arguments):
        return None
    return output_argument(arguments, directory)


def _write_fragment(destination: Path, document: dict[str, object]) -> None:
    destination.mkdir(parents=True, exist_ok=True)
    name = f"{os.getpid()}-{uuid.uuid4().hex}.json"
    with tempfile.NamedTemporaryFile(
        mode="w", encoding="utf-8", dir=destination, prefix=".tmp-", delete=False
    ) as handle:
        json.dump(document, handle, sort_keys=True)
        handle.write("\n")
        temporary = Path(handle.name)
    temporary.replace(destination / name)


def record_compile(
    destination: Path, real_compiler: str, arguments: list[str], directory: Path
) -> None:
    for source in source_arguments(arguments, directory):
        _write_fragment(
            destination,
            {
                "directory": str(directory),
                "file": str(source),
                "arguments": [real_compiler, *arguments],
            },
        )


def record_link(
    destination: Path, real_compiler: str, arguments: list[str], directory: Path
) -> None:
    output = link_output(arguments, directory)
    if output is None:
        return
    _write_fragment(
        destination,
        {
            "schema_version": 1,
            "kind": LINK_FRAGMENT_KIND,
            "directory": str(directory),
            "output": str(output),
            "arguments": [real_compiler, *arguments],
        },
    )


def record(real_compiler: str, arguments: list[str]) -> None:
    directory = Path.cwd().resolve()
    if capture := os.environ.get("POSTGAMMA_CAPTURE_DIR"):
        record_compile(Path(capture).resolve(), real_compiler, arguments, directory)
    if capture := os.environ.get("POSTGAMMA_LINK_CAPTURE_DIR"):
        record_link(Path(capture).resolve(), real_compiler, arguments, directory)


def main() -> None:
    real_compiler = os.environ.get("POSTGAMMA_REAL_CC", "cc")
    arguments = [*compiler_arguments_from_environment(os.environ), *sys.argv[1:]]
    record(real_compiler, arguments)
    os.execvp(real_compiler, [real_compiler, *arguments])


if __name__ == "__main__":
    main()
