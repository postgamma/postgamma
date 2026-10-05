#!/usr/bin/env python3
"""Prepare PostgreSQL's generated BKI stream for the embedded bootstrap proof."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import tempfile
from pathlib import Path


REPORT_KIND = "postgamma.bootstrap-input"
SCHEMA_VERSION = 1
EXPECTED_MAJOR = 19


class BootstrapInputError(ValueError):
    """The generated PostgreSQL bootstrap input is incompatible or stale."""


def sha256_bytes(content: bytes) -> str:
    return hashlib.sha256(content).hexdigest()


def parse_define(path: Path, name: str) -> int:
    pattern = re.compile(rf"^\s*#\s*define\s+{re.escape(name)}\s+(\d+)\s*$")
    matches: list[int] = []
    for line in path.read_text(encoding="utf-8").splitlines():
        match = pattern.match(line)
        if match:
            matches.append(int(match.group(1)))
    if len(matches) != 1 or matches[0] <= 0:
        raise BootstrapInputError(
            f"{path}: expected exactly one positive definition of {name}"
        )
    return matches[0]


def parse_enum_value(path: Path, enum_name: str, member: str) -> int:
    content = path.read_text(encoding="utf-8")
    match = re.search(
        rf"typedef\s+enum\s+{re.escape(enum_name)}\s*\{{(.*?)\}}\s*{re.escape(enum_name)}\s*;",
        content,
        flags=re.DOTALL,
    )
    if match is None:
        raise BootstrapInputError(f"{path}: enum {enum_name} was not found")
    body = re.sub(r"/\*.*?\*/", "", match.group(1), flags=re.DOTALL)
    value = -1
    found: int | None = None
    for raw_entry in body.split(","):
        entry = raw_entry.strip()
        if not entry:
            continue
        parts = [part.strip() for part in entry.split("=", maxsplit=1)]
        if len(parts) == 2:
            try:
                value = int(parts[1], 0)
            except ValueError as exc:
                raise BootstrapInputError(
                    f"{path}: enum expression for {parts[0]} is not an integer"
                ) from exc
        else:
            value += 1
        if parts[0] == member:
            found = value
    if found is None or found < 0:
        raise BootstrapInputError(f"{path}: enum member {member} was not found")
    return found


def replace_token(content: str, token: str, replacement: str) -> tuple[str, int]:
    """Match initdb's whitespace-delimited, first-occurrence-per-line rules."""

    output: list[str] = []
    count = 0
    for line in content.splitlines(keepends=True):
        offset = line.find(token)
        end = offset + len(token)
        if (
            offset >= 0
            and (offset == 0 or line[offset - 1].isspace())
            and (end == len(line) or line[end].isspace())
        ):
            line = line[:offset] + replacement + line[end:]
            count += 1
        output.append(line)
    return "".join(output), count


def has_replaceable_token(content: str, token: str) -> bool:
    _ignored, count = replace_token(content, token, token)
    return count != 0


def prepare(
    bki_path: Path,
    config_header: Path,
    manual_header: Path,
    encoding_header: Path,
    output: Path,
    report_path: Path,
) -> dict[str, object]:
    source = bki_path.read_bytes()
    try:
        content = source.decode("utf-8")
    except UnicodeDecodeError as exc:
        raise BootstrapInputError(f"{bki_path}: BKI input is not UTF-8") from exc
    lines = content.splitlines()
    if not lines or lines[0] != f"# PostgreSQL {EXPECTED_MAJOR}":
        raise BootstrapInputError(
            f"{bki_path}: expected a PostgreSQL {EXPECTED_MAJOR} BKI header"
        )

    pointer_size = parse_define(config_header, "SIZEOF_VOID_P")
    if pointer_size not in (4, 8):
        raise BootstrapInputError(
            f"{config_header}: unsupported SIZEOF_VOID_P {pointer_size}"
        )
    name_length = parse_define(manual_header, "NAMEDATALEN")
    encoding_id = parse_enum_value(encoding_header, "pg_enc", "PG_UTF8")
    replacements = {
        "ALIGNOF_POINTER": "i" if pointer_size == 4 else "d",
        "DATLOCALE": "_null_",
        "ENCODING": str(encoding_id),
        "ICU_RULES": "_null_",
        "LC_COLLATE": "C",
        "LC_CTYPE": "C",
        "LOCALE_PROVIDER": "c",
        "NAMEDATALEN": str(name_length),
        "POSTGRES": "postgamma",
        "SIZEOF_POINTER": str(pointer_size),
    }
    counts: dict[str, int] = {}
    prepared = content
    for token, replacement in sorted(replacements.items()):
        prepared, count = replace_token(prepared, token, replacement)
        if count == 0:
            raise BootstrapInputError(f"{bki_path}: token {token} was not present")
        counts[token] = count
    remaining = [
        token for token in replacements if has_replaceable_token(prepared, token)
    ]
    if remaining:
        raise BootstrapInputError(
            f"{bki_path}: unexpanded token(s): " + ", ".join(sorted(remaining))
        )

    prepared_bytes = prepared.encode("utf-8")
    write_bytes(output, prepared_bytes)
    document: dict[str, object] = {
        "schema_version": SCHEMA_VERSION,
        "kind": REPORT_KIND,
        "postgresql_major": EXPECTED_MAJOR,
        "source": str(bki_path.resolve()),
        "source_sha256": sha256_bytes(source),
        "output": str(output.resolve()),
        "output_sha256": sha256_bytes(prepared_bytes),
        "configuration": {
            "name_data_length": name_length,
            "pointer_size": pointer_size,
            "utf8_encoding_id": encoding_id,
        },
        "replacements": {
            token: {"count": counts[token], "value": replacements[token]}
            for token in sorted(replacements)
        },
    }
    write_json(report_path, document)
    return document


def write_json(path: Path, document: dict[str, object]) -> None:
    content = json.dumps(document, indent=2, sort_keys=True) + "\n"
    path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(
        mode="w",
        encoding="utf-8",
        dir=path.parent,
        prefix=f".{path.name}.",
        delete=False,
    ) as handle:
        handle.write(content)
        temporary = Path(handle.name)
    try:
        os.replace(temporary, path)
    finally:
        temporary.unlink(missing_ok=True)


def write_bytes(path: Path, content: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(
        mode="wb",
        dir=path.parent,
        prefix=f".{path.name}.",
        delete=False,
    ) as handle:
        handle.write(content)
        temporary = Path(handle.name)
    try:
        os.replace(temporary, path)
    finally:
        temporary.unlink(missing_ok=True)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--bki", required=True, type=Path)
    parser.add_argument("--config-header", required=True, type=Path)
    parser.add_argument("--manual-header", required=True, type=Path)
    parser.add_argument("--encoding-header", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--report", required=True, type=Path)
    args = parser.parse_args()
    try:
        document = prepare(
            args.bki.resolve(),
            args.config_header.resolve(),
            args.manual_header.resolve(),
            args.encoding_header.resolve(),
            args.output.resolve(),
            args.report.resolve(),
        )
    except (BootstrapInputError, OSError) as exc:
        parser.error(str(exc))
    replacements = document["replacements"]
    assert isinstance(replacements, dict)
    print(
        f"embedded bootstrap input: PostgreSQL {EXPECTED_MAJOR}, "
        f"{sum(item['count'] for item in replacements.values())} replacement(s)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
