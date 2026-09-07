#!/usr/bin/env python3
"""Derive PostgreSQL backend execution types from proctypelist.h."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path


class CatalogError(ValueError):
    """The upstream backend type list cannot be parsed unambiguously."""


PREFIX = "PG_PROCTYPE("
PARSER_ID = "postgresql.proctypelist.v1"


def split_arguments(value: str) -> list[str]:
    arguments: list[str] = []
    current: list[str] = []
    depth = 0
    quoted = False
    escaped = False
    for character in value:
        if quoted:
            current.append(character)
            if escaped:
                escaped = False
            elif character == "\\":
                escaped = True
            elif character == '"':
                quoted = False
            continue
        if character == '"':
            quoted = True
            current.append(character)
        elif character == "(":
            depth += 1
            current.append(character)
        elif character == ")":
            if depth == 0:
                raise CatalogError("unbalanced closing parenthesis")
            depth -= 1
            current.append(character)
        elif character == "," and depth == 0:
            arguments.append("".join(current).strip())
            current = []
        else:
            current.append(character)
    if quoted or depth != 0:
        raise CatalogError("unterminated string or parenthesized expression")
    arguments.append("".join(current).strip())
    return arguments


def extract_description(expression: str) -> str:
    prefix = 'gettext_noop("'
    if not expression.startswith(prefix) or not expression.endswith('")'):
        raise CatalogError(f"unsupported description expression: {expression}")
    encoded = '"' + expression[len(prefix) : -1]
    try:
        value = json.loads(encoded)
    except json.JSONDecodeError as exc:
        raise CatalogError(f"invalid description string: {expression}") from exc
    if not isinstance(value, str) or not value:
        raise CatalogError("backend execution description must not be empty")
    return value


def parse_proctypelist(path: Path) -> list[dict[str, object]]:
    try:
        text = path.read_text(encoding="utf-8")
    except OSError as exc:
        raise CatalogError(f"cannot read {path}: {exc}") from exc

    executions: list[dict[str, object]] = []
    symbols: set[str] = set()
    for line_number, raw_line in enumerate(text.splitlines(), 1):
        line = raw_line.strip()
        if not line.startswith(PREFIX):
            continue
        if not line.endswith(")"):
            raise CatalogError(f"{path}:{line_number}: unterminated PG_PROCTYPE")
        arguments = split_arguments(line[len(PREFIX) : -1])
        if len(arguments) != 5:
            raise CatalogError(
                f"{path}:{line_number}: expected five PG_PROCTYPE arguments, "
                f"found {len(arguments)}"
            )
        symbol, category_expression, description_expression, main_function, attach = (
            arguments
        )
        if not symbol.startswith("B_") or not symbol.replace("_", "").isalnum():
            raise CatalogError(f"{path}:{line_number}: invalid backend symbol {symbol!r}")
        if symbol in symbols:
            raise CatalogError(f"{path}:{line_number}: duplicate backend symbol {symbol}")
        symbols.add(symbol)
        try:
            category = json.loads(category_expression)
        except json.JSONDecodeError as exc:
            raise CatalogError(
                f"{path}:{line_number}: invalid category {category_expression!r}"
            ) from exc
        if not isinstance(category, str) or not category:
            raise CatalogError(f"{path}:{line_number}: category must not be empty")
        if attach not in {"true", "false"}:
            raise CatalogError(
                f"{path}:{line_number}: shmem_attach must be true or false"
            )
        if main_function != "NULL" and not main_function.replace("_", "").isalnum():
            raise CatalogError(
                f"{path}:{line_number}: invalid main function {main_function!r}"
            )
        executions.append(
            {
                "symbol": symbol,
                "category": category,
                "description": extract_description(description_expression),
                "main_function": None if main_function == "NULL" else main_function,
                "shared_memory_access": attach == "true",
                "source_line": line_number,
            }
        )
    if not executions:
        raise CatalogError(f"{path}: no PG_PROCTYPE entries found")
    return sorted(executions, key=lambda entry: str(entry["symbol"]))


def compile_catalog(
    path: Path, source_label: str, parser_id: str = PARSER_ID
) -> dict[str, object]:
    if parser_id != PARSER_ID:
        raise CatalogError(
            f"unsupported backend execution fact parser {parser_id!r}"
        )
    return {
        "schema_version": 1,
        "kind": "postgamma.backend-execution-catalog",
        "source": {
            "path": source_label,
            "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
        },
        "executions": parse_proctypelist(path),
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", required=True, type=Path)
    parser.add_argument("--source-label", required=True)
    parser.add_argument("--parser", required=True)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    try:
        catalog = compile_catalog(
            args.input.resolve(), args.source_label, args.parser
        )
    except CatalogError as exc:
        parser.error(str(exc))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(
        json.dumps(catalog, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    print(
        f"backend execution catalog: {len(catalog['executions'])} types -> "
        f"{args.output}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
