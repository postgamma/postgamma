#!/usr/bin/env python3
"""Emit deterministic facts for every PostgreSQL built-in GUC.

PostgreSQL's ``guc_parameters.dat`` is the upstream source of truth.  This
generator deliberately records facts only; ownership remains a reviewed
PostGamma policy decision.

The upstream file uses a small Perl-data syntax.  Parsing is intentionally
strict: a PostgreSQL upgrade that introduces new syntax must fail here and be
reviewed instead of silently producing an incomplete catalog.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import sys
from pathlib import Path
from typing import Sequence


REQUIRED_FIELDS = ("name", "variable", "type", "context", "group", "boot_val")
HOOK_FIELDS = ("check_hook", "assign_hook", "show_hook")
PARSER_ID = "postgresql.guc-parameters-dat.v1"
LIMIT_FIELDS = ("min", "max", "options")


class CatalogDataError(ValueError):
    """The upstream catalog does not match the supported data syntax."""


class CatalogDataParser:
    """Parse the constrained Perl-data syntax used by guc_parameters.dat."""

    def __init__(self, text: str, source: Path):
        self.text = text
        self.source = source
        self.position = 0

    def location(self, position: int | None = None) -> str:
        offset = self.position if position is None else position
        line = self.text.count("\n", 0, offset) + 1
        previous_newline = self.text.rfind("\n", 0, offset)
        column = offset - previous_newline
        return f"{self.source}:{line}:{column}"

    def fail(self, message: str, position: int | None = None) -> CatalogDataError:
        return CatalogDataError(f"{self.location(position)}: {message}")

    def skip_ignored(self) -> None:
        while self.position < len(self.text):
            if self.text[self.position].isspace():
                self.position += 1
                continue
            if self.text[self.position] == "#":
                newline = self.text.find("\n", self.position)
                self.position = len(self.text) if newline < 0 else newline + 1
                continue
            if self.text.startswith("/*", self.position):
                comment_start = self.position
                comment_end = self.text.find("*/", self.position + 2)
                if comment_end < 0:
                    raise self.fail("unterminated block comment", comment_start)
                self.position = comment_end + 2
                continue
            return

    def accept(self, token: str) -> bool:
        self.skip_ignored()
        if self.text.startswith(token, self.position):
            self.position += len(token)
            return True
        return False

    def expect(self, token: str) -> None:
        if not self.accept(token):
            raise self.fail(f"expected {token!r}")

    def parse_identifier(self) -> str:
        self.skip_ignored()
        start = self.position
        if start >= len(self.text) or not (
            self.text[start].isalpha() or self.text[start] == "_"
        ):
            raise self.fail("expected field name")
        self.position += 1
        while self.position < len(self.text):
            character = self.text[self.position]
            if not (character.isalnum() or character == "_"):
                break
            self.position += 1
        return self.text[start : self.position]

    def parse_single_quoted_string(self) -> str:
        self.skip_ignored()
        start = self.position
        if not self.accept("'"):
            raise self.fail("expected a single-quoted string", start)

        value: list[str] = []
        while self.position < len(self.text):
            character = self.text[self.position]
            self.position += 1
            if character == "'":
                return "".join(value)
            if character != "\\":
                value.append(character)
                continue
            if self.position >= len(self.text):
                raise self.fail("unterminated escape in single-quoted string", start)
            escaped = self.text[self.position]
            self.position += 1
            if escaped in ("\\", "'"):
                value.append(escaped)
            else:
                # Perl single-quoted strings preserve all other backslashes.
                value.extend(("\\", escaped))
        raise self.fail("unterminated single-quoted string", start)

    def parse_record(self) -> tuple[dict[str, str], int]:
        self.skip_ignored()
        record_position = self.position
        self.expect("{")
        fields: dict[str, str] = {}
        while True:
            self.skip_ignored()
            if self.accept("}"):
                return fields, self.text.count("\n", 0, record_position) + 1

            field_position = self.position
            field = self.parse_identifier()
            if field in fields:
                raise self.fail(f"duplicate field {field}", field_position)
            self.expect("=>")
            fields[field] = self.parse_single_quoted_string()

            self.skip_ignored()
            if self.accept(","):
                continue
            if not self.text.startswith("}", self.position):
                raise self.fail("expected ',' or '}' after field value")

    def parse(self) -> list[tuple[dict[str, str], int]]:
        self.expect("[")
        entries: list[tuple[dict[str, str], int]] = []
        while True:
            self.skip_ignored()
            if self.accept("]"):
                break
            entries.append(self.parse_record())
            self.skip_ignored()
            if self.accept(","):
                continue
            if not self.text.startswith("]", self.position):
                raise self.fail("expected ',' or ']' after record")

        self.skip_ignored()
        if self.position != len(self.text):
            raise self.fail("unexpected content after top-level array")
        return entries


def parse_catalog_data(data: bytes, source: Path) -> list[tuple[dict[str, str], int]]:
    try:
        text = data.decode("utf-8")
    except UnicodeDecodeError as exc:
        raise CatalogDataError(f"{source}: input is not UTF-8: {exc}") from exc
    return CatalogDataParser(text, source).parse()


def catalog_document(
    entries: Sequence[tuple[dict[str, str], int]],
    input_bytes: bytes,
    input_path: Path,
    source_label: str,
) -> dict[str, object]:
    parameters: list[dict[str, object]] = []
    names: set[str] = set()
    storage_symbols: set[str] = set()

    for entry, line_number in entries:
        for required in REQUIRED_FIELDS:
            if required not in entry:
                raise CatalogDataError(
                    f"{input_path}:{line_number}: missing {required}"
                )
        name = entry["name"]
        storage_symbol = entry["variable"]
        if name in names:
            raise CatalogDataError(
                f"{input_path}:{line_number}: duplicate GUC name {name}"
            )
        if storage_symbol in storage_symbols:
            raise CatalogDataError(
                f"{input_path}:{line_number}: duplicate storage symbol {storage_symbol}"
            )
        names.add(name)
        storage_symbols.add(storage_symbol)

        parameters.append(
            {
                "name": name,
                "storage_symbol": storage_symbol,
                "value_type": entry["type"],
                "context": entry["context"],
                "group": entry["group"],
                "boot_value": entry["boot_val"],
                "flags": entry.get("flags", ""),
                "compile_condition": entry.get("ifdef", ""),
                "hooks": {key: entry[key] for key in HOOK_FIELDS if key in entry},
                "limits": {key: entry[key] for key in LIMIT_FIELDS if key in entry},
            }
        )

    parameters.sort(key=lambda parameter: str(parameter["name"]))
    return {
        "schema_version": 1,
        "kind": "postgamma.postgres-guc-catalog",
        "source": {
            "path": source_label,
            "sha256": hashlib.sha256(input_bytes).hexdigest(),
        },
        "parameter_count": len(parameters),
        "parameters": parameters,
    }


def encode_catalog_json(value: object, level: int = 0) -> str:
    """Preserve the canonical pretty layout of previously generated catalogs."""

    indentation = " " * (level * 3)
    child_indentation = " " * ((level + 1) * 3)
    if isinstance(value, dict):
        if not value:
            return "{}"
        lines = ["{"]
        items = sorted(value.items())
        for index, (key, child) in enumerate(items):
            if not isinstance(key, str):
                raise TypeError("JSON object keys must be strings")
            encoded = encode_catalog_json(child, level + 1).splitlines()
            prefix = f"{child_indentation}{json.dumps(key)} : "
            lines.append(prefix + encoded[0])
            lines.extend(encoded[1:])
            if index + 1 < len(items):
                lines[-1] += ","
        lines.append(indentation + "}")
        return "\n".join(lines)
    if isinstance(value, list):
        if not value:
            return "[]"
        lines = ["["]
        for index, child in enumerate(value):
            encoded = encode_catalog_json(child, level + 1).splitlines()
            lines.append(child_indentation + encoded[0])
            lines.extend(encoded[1:])
            if index + 1 < len(value):
                lines[-1] += ","
        lines.append(indentation + "]")
        return "\n".join(lines)
    if value is None or isinstance(value, (str, int, float, bool)):
        return json.dumps(value, ensure_ascii=True, allow_nan=False)
    raise TypeError(f"unsupported JSON value: {type(value).__name__}")


def write_atomic(path: Path, data: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(f"{path.name}.tmp.{os.getpid()}")
    try:
        temporary.write_bytes(data)
        os.replace(temporary, path)
    finally:
        temporary.unlink(missing_ok=True)


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--source-label", required=True)
    parser.add_argument("--parser", required=True)
    args = parser.parse_args(argv)

    try:
        if args.parser != PARSER_ID:
            raise CatalogDataError(f"unsupported GUC fact parser {args.parser!r}")
        input_path = args.input.resolve(strict=True)
        input_bytes = input_path.read_bytes()
        entries = parse_catalog_data(input_bytes, input_path)
        document = catalog_document(
            entries, input_bytes, input_path, args.source_label
        )
        output_path = args.output.resolve()
        write_atomic(
            output_path, (encode_catalog_json(document) + "\n").encode("utf-8")
        )
    except (CatalogDataError, OSError, TypeError) as exc:
        print(f"generate_guc_catalog.py: error: {exc}", file=sys.stderr)
        return 1

    print(f"GUC catalog: {len(entries)} parameters -> {output_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
