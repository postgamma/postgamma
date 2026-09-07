#!/usr/bin/env python3
"""Fail-closed structural probes for small PostgreSQL C integration seams."""

from __future__ import annotations

import re
from pathlib import Path
from typing import Any


class CSourceProbeError(ValueError):
    """A declared C source seam drifted from its reviewed structure."""


def sanitize_c(source: str) -> str:
    result = list(source)
    index = 0
    state = "code"
    while index < len(result):
        current = result[index]
        following = result[index + 1] if index + 1 < len(result) else ""
        if state == "code":
            if current == "/" and following == "*":
                result[index] = result[index + 1] = " "
                state = "block-comment"
                index += 2
                continue
            if current == "/" and following == "/":
                result[index] = result[index + 1] = " "
                state = "line-comment"
                index += 2
                continue
            if current in {'"', "'"}:
                quote = current
                result[index] = " "
                state = quote
        elif state == "block-comment":
            if current == "*" and following == "/":
                result[index] = result[index + 1] = " "
                state = "code"
                index += 2
                continue
            if current != "\n":
                result[index] = " "
        elif state == "line-comment":
            if current == "\n":
                state = "code"
            else:
                result[index] = " "
        else:
            if current == "\\" and following:
                result[index] = " "
                if following != "\n":
                    result[index + 1] = " "
                index += 2
                continue
            if current == state:
                state = "code"
            if current != "\n":
                result[index] = " "
        index += 1
    return "".join(result)


def function_span(
    source: str, name: str, expected_definitions: int
) -> tuple[int, int]:
    function = re.escape(name)
    pattern = re.compile(
        rf"^[ \t]*{function}\s*\([^;{{}}]*\)\s*\{{",
        re.DOTALL | re.MULTILINE,
    )
    matches = list(pattern.finditer(source))
    if len(matches) != expected_definitions:
        raise CSourceProbeError(
            f"expected {expected_definitions} definition(s) of {name}, "
            f"found {len(matches)}"
        )
    start = matches[0].end() - 1
    depth = 0
    for index in range(start, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return start, index + 1
    raise CSourceProbeError(f"function body for {name} is unbalanced")


def function_body(source: str, name: str, expected_definitions: int) -> str:
    start, end = function_span(source, name, expected_definitions)
    return source[start:end]


def verify_source_seam(source_root: Path, seam: dict[str, Any]) -> dict[str, Any]:
    source_path = source_root / seam["source_file"]
    try:
        source = sanitize_c(source_path.read_text(encoding="utf-8"))
    except OSError as exc:
        raise CSourceProbeError(f"cannot read source seam {source_path}: {exc}") from exc
    body = function_body(
        source, seam["enclosing_function"], seam["expected_definitions"]
    )
    counts: dict[str, int] = {}
    for callee in seam["required_callees"]:
        count = len(re.findall(rf"\b{re.escape(callee['name'])}\s*\(", body))
        if count != callee["expected_matches"]:
            raise CSourceProbeError(
                f"source seam callee {callee['name']} expected "
                f"{callee['expected_matches']} match(es), found {count}"
            )
        counts[callee["name"]] = count
    return {
        "source_file": seam["source_file"],
        "enclosing_function": seam["enclosing_function"],
        "definition_matches": seam["expected_definitions"],
        "callee_matches": dict(sorted(counts.items())),
    }
