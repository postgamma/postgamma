#!/usr/bin/env python3
"""Derive one exact source domain from PostgreSQL compile commands."""

from __future__ import annotations

import argparse
import json
import re
import shlex
from pathlib import Path
from typing import Any

from list_compile_sources import in_translation_unit_domain
from merge_compile_db import canonicalize_source_argument, source_root_kind


FRONTEND_INCLUDE = re.compile(
    rb"^[ \t]*#[ \t]*include[ \t]*[<\"]postgres_fe\.h[>\"]",
    re.MULTILINE,
)
BACKEND_INCLUDE = re.compile(
    rb"^[ \t]*#[ \t]*include[ \t]*[<\"]postgres\.h[>\"]",
    re.MULTILINE,
)
MACRO_DEFINITION = re.compile(
    rb"^[ \t]*#[ \t]*define[ \t]+([A-Za-z_][A-Za-z0-9_]*)",
    re.MULTILINE,
)
DOMAIN_KIND = "postgamma.postgres-source-domain"
DOMAIN_FIELDS = frozenset(
    {
        "schema_version",
        "kind",
        "id",
        "description",
        "translation_unit_prefixes",
        "translation_unit_files",
        "required_defines",
        "required_any_defines",
        "excluded_defines",
        "required_output_suffixes",
        "excluded_output_suffixes",
        "binding_anchor_file_suffixes",
    }
)


def record_arguments(record: dict[str, Any]) -> list[str]:
    arguments = record.get("arguments")
    if isinstance(arguments, list) and all(isinstance(item, str) for item in arguments):
        return arguments
    command = record.get("command")
    if isinstance(command, str):
        return shlex.split(command)
    raise ValueError("compile database record has neither arguments nor command")


def defined_macros(arguments: list[str]) -> set[str]:
    result: set[str] = set()
    index = 0
    while index < len(arguments):
        argument = arguments[index]
        value: str | None = None
        if argument == "-D" and index + 1 < len(arguments):
            index += 1
            value = arguments[index]
        elif argument.startswith("-D") and len(argument) > 2:
            value = argument[2:]
        if value:
            result.add(value.split("=", 1)[0])
        index += 1
    return result


def source_defined_macros(source: Path) -> set[str]:
    """Return domain macros established directly by a translation unit."""

    try:
        data = source.read_bytes()
    except OSError as exc:
        raise ValueError(f"cannot read source-domain input {source}: {exc}") from exc
    result = {
        match.group(1).decode("ascii") for match in MACRO_DEFINITION.finditer(data)
    }
    # PostgreSQL's frontend umbrella header establishes FRONTEND internally,
    # so it is intentionally absent from many compile command lines.  Common
    # sources can select postgres.h or postgres_fe.h with #ifndef FRONTEND;
    # their command-line profile remains authoritative.
    if FRONTEND_INCLUDE.search(data) and not BACKEND_INCLUDE.search(data):
        result.add("FRONTEND")
    return result


def _domain_string_array(document: dict[str, Any], field: str) -> list[str]:
    value = document.get(field)
    if not isinstance(value, list) or not all(
        isinstance(item, str) and item for item in value
    ):
        raise ValueError(f"domain {field} must be an array of non-empty strings")
    if len(value) != len(set(value)):
        raise ValueError(f"domain {field} contains duplicate values")
    return sorted(value)


def parse_domain_manifest(document: dict[str, Any]) -> dict[str, Any]:
    unknown = sorted(set(document) - DOMAIN_FIELDS)
    if unknown:
        raise ValueError("domain has unknown field(s): " + ", ".join(unknown))
    if document.get("schema_version") != 1 or document.get("kind") != DOMAIN_KIND:
        raise ValueError(f"domain must use schema_version 1 and kind {DOMAIN_KIND}")
    identifier = document.get("id")
    description = document.get("description")
    if not isinstance(identifier, str) or not re.fullmatch(
        r"[a-z][a-z0-9]*(?:-[a-z0-9]+)*", identifier
    ):
        raise ValueError("domain id must be a lowercase hyphenated identifier")
    if not isinstance(description, str) or not description:
        raise ValueError("domain description must be a non-empty string")
    parsed = {
        **document,
        "translation_unit_prefixes": _domain_string_array(
            document, "translation_unit_prefixes"
        ),
        "translation_unit_files": _domain_string_array(
            document, "translation_unit_files"
        ),
        "required_defines": _domain_string_array(document, "required_defines"),
        "required_any_defines": _domain_string_array(
            document, "required_any_defines"
        ),
        "excluded_defines": _domain_string_array(document, "excluded_defines"),
        "required_output_suffixes": _domain_string_array(
            document, "required_output_suffixes"
        ),
        "excluded_output_suffixes": _domain_string_array(
            document, "excluded_output_suffixes"
        ),
        "binding_anchor_file_suffixes": _domain_string_array(
            document, "binding_anchor_file_suffixes"
        ),
    }
    if not parsed["translation_unit_prefixes"] and not parsed["translation_unit_files"]:
        raise ValueError("domain must select at least one translation-unit path")
    overlap = set(parsed["required_defines"]) & set(parsed["excluded_defines"])
    if overlap:
        raise ValueError(
            "domain requires and excludes the same define(s): "
            + ", ".join(sorted(overlap))
        )
    any_overlap = set(parsed["required_any_defines"]) & set(
        parsed["excluded_defines"]
    )
    if any_overlap:
        raise ValueError(
            "domain requires-any and excludes the same define(s): "
            + ", ".join(sorted(any_overlap))
        )
    return parsed


def compiler_output(arguments: list[str], directory: Path) -> Path | None:
    outputs: list[str] = []
    index = 0
    while index < len(arguments):
        argument = arguments[index]
        if argument == "-o":
            if index + 1 >= len(arguments):
                raise ValueError("compile command has -o without an output path")
            index += 1
            outputs.append(arguments[index])
        elif argument.startswith("-o") and len(argument) > 2:
            outputs.append(argument[2:])
        index += 1
    if len(outputs) > 1:
        raise ValueError("compile command has more than one output path")
    if not outputs:
        return None
    output = Path(outputs[0])
    return output.resolve() if output.is_absolute() else (directory / output).resolve()


def select_compile_records(
    records: list[Any],
    root: Path,
    generated_root: Path | None,
    manifest: dict[str, Any],
) -> tuple[list[dict[str, Any]], dict[str, Any]]:
    domain = parse_domain_manifest(manifest)
    prefixes = domain["translation_unit_prefixes"]
    files = domain["translation_unit_files"]
    required_defines = set(domain["required_defines"])
    required_any_defines = set(domain["required_any_defines"])
    excluded_defines = set(domain["excluded_defines"])
    required_outputs = domain["required_output_suffixes"]
    excluded_outputs = domain["excluded_output_suffixes"]
    selected: list[tuple[str, str, dict[str, Any], Path | None]] = []
    selected_sources: dict[tuple[str, str], int] = {}
    macro_cache: dict[Path, set[str]] = {}

    for index, record in enumerate(records):
        if not isinstance(record, dict) or not isinstance(record.get("file"), str):
            raise ValueError(f"compile database record {index} is invalid")
        source = Path(record["file"]).resolve()
        root_kind = source_root_kind(source, root, generated_root)
        if root_kind is None:
            continue
        logical_root = root if root_kind == "source" else generated_root
        assert logical_root is not None
        relative = source.relative_to(logical_root).as_posix()
        if not in_translation_unit_domain(relative, prefixes, files):
            continue
        try:
            arguments = record_arguments(record)
            directory_value = record.get("directory")
            if not isinstance(directory_value, str) or not directory_value:
                raise ValueError("directory must be a non-empty string")
            directory = Path(directory_value).resolve()
            output = compiler_output(arguments, directory)
            if source not in macro_cache:
                macro_cache[source] = source_defined_macros(source)
            declared_macros = macro_cache[source]
        except ValueError as exc:
            raise ValueError(f"compile database record {index}: {exc}") from exc
        macros = defined_macros(arguments) | declared_macros
        if not required_defines.issubset(macros):
            continue
        if required_any_defines and not macros & required_any_defines:
            continue
        if macros & excluded_defines:
            continue
        output_text = str(output) if output is not None else ""
        if required_outputs and not any(
            output_text.endswith(suffix) for suffix in required_outputs
        ):
            continue
        if any(output_text.endswith(suffix) for suffix in excluded_outputs):
            continue
        try:
            normalized_arguments = canonicalize_source_argument(
                arguments, directory, source
            )
        except ValueError as exc:
            raise ValueError(f"compile database record {index}: {exc}") from exc
        source_key = (root_kind, relative)
        if source_key in selected_sources:
            raise ValueError(
                f"ambiguous {domain['id']} compile profiles for "
                f"{root_kind}:{relative} (records "
                f"{selected_sources[source_key]} and {index}); add an explicit "
                "domain exclusion instead of relying on compile-database order"
            )
        normalized = {
            "directory": str(directory),
            "file": str(source),
            "arguments": normalized_arguments,
        }
        selected.append((root_kind, relative, normalized, output))
        selected_sources[source_key] = index

    if not selected:
        raise ValueError(f"{domain['id']} domain selected no compile commands")
    selected.sort(
        key=lambda item: (
            item[0],
            item[1],
            json.dumps(item[2]["arguments"], sort_keys=True),
            item[2]["directory"],
        )
    )
    report = {
        "schema_version": 1,
        "kind": "postgamma.compile-domain-selection",
        "domain": domain["id"],
        "required_defines": domain["required_defines"],
        "required_any_defines": domain["required_any_defines"],
        "excluded_defines": domain["excluded_defines"],
        "required_output_suffixes": domain["required_output_suffixes"],
        "excluded_output_suffixes": domain["excluded_output_suffixes"],
        "selected_translation_units": len(selected),
        "files": [
            {
                "root": root_kind,
                "path": relative,
                "output": str(output) if output is not None else None,
            }
            for root_kind, relative, _record, output in selected
        ],
    }
    return [record for _kind, _relative, record, _output in selected], report


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--database", required=True, type=Path)
    parser.add_argument("--source-root", required=True, type=Path)
    parser.add_argument("--generated-root", type=Path)
    parser.add_argument("--domain-manifest", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--selection-report", type=Path)
    args = parser.parse_args()

    root = args.source_root.resolve()
    generated_root = args.generated_root.resolve() if args.generated_root else None
    records = json.loads(args.database.read_text(encoding="utf-8"))
    manifest = json.loads(args.domain_manifest.read_text(encoding="utf-8"))
    if not isinstance(records, list) or not isinstance(manifest, dict):
        parser.error("compile database must be an array and domain must be an object")
    try:
        selected, report = select_compile_records(
            records, root, generated_root, manifest
        )
    except ValueError as exc:
        parser.error(str(exc))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(
        json.dumps(selected, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    if args.selection_report:
        args.selection_report.parent.mkdir(parents=True, exist_ok=True)
        args.selection_report.write_text(
            json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8"
        )
    print(
        f"{report['domain']} compile database: {len(selected)} commands, "
        f"{report['selected_translation_units']} translation units -> {args.output}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
