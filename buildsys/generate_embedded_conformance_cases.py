#!/usr/bin/env python3
"""Validate the embedded SQL corpus and generate its dependency-free C table."""

from __future__ import annotations

import argparse
import json
import os
import re
import tempfile
from collections import Counter
from pathlib import Path
from typing import Any


CORPUS_KIND = "postgamma.embedded-conformance-corpus"
NAME = re.compile(r"^[a-z][a-z0-9_]*$")
SQLSTATE = re.compile(r"^[0-9A-Z]{5}$")
VALID_SESSIONS = {"primary", "secondary"}
VALID_KINDS = {"command", "tuples", "error"}


class ConformanceCorpusError(RuntimeError):
    """The declarative embedded conformance corpus is malformed."""


def reject_duplicate_keys(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for name, value in pairs:
        if name in result:
            raise ConformanceCorpusError(f"duplicate JSON key: {name}")
        result[name] = value
    return result


def load_corpus(path: Path) -> dict[str, Any]:
    try:
        document = json.loads(
            path.read_text(encoding="utf-8"),
            object_pairs_hook=reject_duplicate_keys,
        )
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise ConformanceCorpusError(f"cannot read corpus {path}: {error}") from error
    if not isinstance(document, dict):
        raise ConformanceCorpusError("corpus root must be an object")
    allowed_root = {
        "schema_version",
        "kind",
        "postgresql_major",
        "required_categories",
        "cases",
    }
    unknown_root = sorted(set(document) - allowed_root)
    if unknown_root:
        raise ConformanceCorpusError(
            "unknown corpus fields: " + ", ".join(unknown_root)
        )
    if (
        document.get("schema_version") != 1
        or document.get("kind") != CORPUS_KIND
        or document.get("postgresql_major") != 19
    ):
        raise ConformanceCorpusError("corpus identity must describe PostgreSQL 19")
    required = document.get("required_categories")
    cases = document.get("cases")
    if (
        not isinstance(required, list)
        or not required
        or any(not isinstance(value, str) or NAME.fullmatch(value) is None for value in required)
        or len(required) != len(set(required))
    ):
        raise ConformanceCorpusError("required_categories must be unique identifiers")
    if not isinstance(cases, list) or len(cases) < 40:
        raise ConformanceCorpusError("corpus must contain at least 40 cases")

    allowed_case = {
        "name",
        "category",
        "session",
        "sql",
        "expected_kind",
        "expected_sqlstate",
    }
    names: set[str] = set()
    categories: Counter[str] = Counter()
    for index, case in enumerate(cases):
        if not isinstance(case, dict):
            raise ConformanceCorpusError(f"case {index} must be an object")
        unknown = sorted(set(case) - allowed_case)
        if unknown:
            raise ConformanceCorpusError(
                f"case {index} has unknown fields: " + ", ".join(unknown)
            )
        name = case.get("name")
        category = case.get("category")
        session = case.get("session")
        sql = case.get("sql")
        kind = case.get("expected_kind")
        sqlstate = case.get("expected_sqlstate", "")
        if not isinstance(name, str) or NAME.fullmatch(name) is None:
            raise ConformanceCorpusError(f"case {index} has an invalid name")
        if name in names:
            raise ConformanceCorpusError(f"duplicate case name: {name}")
        names.add(name)
        if category not in required:
            raise ConformanceCorpusError(f"case {name} has an unknown category")
        categories[category] += 1
        if session not in VALID_SESSIONS:
            raise ConformanceCorpusError(f"case {name} has an invalid session")
        if (
            not isinstance(sql, str)
            or not sql.strip()
            or "\x00" in sql
            or not sql.isascii()
        ):
            raise ConformanceCorpusError(
                f"case {name} SQL must be nonempty ASCII without NUL"
            )
        if kind not in VALID_KINDS:
            raise ConformanceCorpusError(f"case {name} has an invalid result kind")
        if kind == "error":
            if not isinstance(sqlstate, str) or SQLSTATE.fullmatch(sqlstate) is None:
                raise ConformanceCorpusError(
                    f"error case {name} requires a five-character SQLSTATE"
                )
        elif "expected_sqlstate" in case:
            raise ConformanceCorpusError(
                f"non-error case {name} may not declare expected_sqlstate"
            )
    missing = [name for name in required if categories[name] == 0]
    if missing:
        raise ConformanceCorpusError(
            "required categories without cases: " + ", ".join(missing)
        )
    return document


def c_string(value: str) -> str:
    encoded = value.encode("ascii")
    output = ['"']
    for byte in encoded:
        if byte == ord("\\"):
            output.append("\\\\")
        elif byte == ord('"'):
            output.append('\\"')
        elif byte == ord("\n"):
            output.append("\\n")
        elif byte == ord("\r"):
            output.append("\\r")
        elif byte == ord("\t"):
            output.append("\\t")
        elif 32 <= byte <= 126:
            output.append(chr(byte))
        else:
            output.append(f"\\{byte:03o}")
    output.append('"')
    return "".join(output)


def render_header(document: dict[str, Any]) -> str:
    cases = document["cases"]
    lines = [
        "/* Generated from manifests/tests/embedded-conformance-v1.json. */",
        "#ifndef POSTGAMMA_EMBEDDED_CONFORMANCE_CASES_H",
        "#define POSTGAMMA_EMBEDDED_CONFORMANCE_CASES_H",
        "",
        "#include <stddef.h>",
        "",
        "typedef struct postgamma_conformance_case",
        "{",
        "\tconst char *name;",
        "\tconst char *category;",
        "\tconst char *session;",
        "\tconst char *sql;",
        "\tconst char *expected_kind;",
        "\tconst char *expected_sqlstate;",
        "} postgamma_conformance_case;",
        "",
        "static const postgamma_conformance_case postgamma_conformance_cases[] =",
        "{",
    ]
    for case in cases:
        values = (
            case["name"],
            case["category"],
            case["session"],
            case["sql"],
            case["expected_kind"],
            case.get("expected_sqlstate", ""),
        )
        lines.extend(
            (
                "\t{",
                "\t\t" + ", ".join(c_string(value) for value in values),
                "\t},",
            )
        )
    lines.extend(
        (
            "};",
            "",
            "#define POSTGAMMA_CONFORMANCE_CASE_COUNT \\",
            "\t(sizeof(postgamma_conformance_cases) / \\",
            "\t sizeof(postgamma_conformance_cases[0]))",
            "",
            "#endif",
            "",
        )
    )
    return "\n".join(lines)


def atomic_write(path: Path, content: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary_name = tempfile.mkstemp(prefix=f".{path.name}.", dir=path.parent)
    temporary = Path(temporary_name)
    try:
        with os.fdopen(descriptor, "w", encoding="utf-8", newline="\n") as stream:
            stream.write(content)
        os.replace(temporary, path)
    finally:
        temporary.unlink(missing_ok=True)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--corpus", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    try:
        document = load_corpus(args.corpus.resolve(strict=True))
        content = render_header(document)
        output = args.output.resolve()
        if not output.is_file() or output.read_text(encoding="utf-8") != content:
            atomic_write(output, content)
    except (OSError, ConformanceCorpusError) as error:
        parser.error(str(error))
    print(f"embedded conformance corpus: {len(document['cases'])} cases -> {output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
