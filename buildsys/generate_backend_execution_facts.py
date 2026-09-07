#!/usr/bin/env python3
"""Emit C preprocessor facts from the reviewed backend execution alignment."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import Any


ALIGNMENT_KIND = "postgamma.backend-execution-policy-alignment"


class FactError(ValueError):
    """The reviewed execution alignment cannot be represented as C facts."""


def load_alignment(path: Path) -> dict[str, Any]:
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise FactError(f"cannot load {path}: {exc}") from exc
    if (
        not isinstance(document, dict)
        or document.get("schema_version") != 1
        or document.get("kind") != ALIGNMENT_KIND
    ):
        raise FactError(
            f"{path}: expected schema_version 1 and kind {ALIGNMENT_KIND}"
        )
    return document


def emit_facts(alignment: dict[str, Any]) -> str:
    executions = alignment.get("executions")
    if not isinstance(executions, list) or not executions:
        raise FactError("alignment.executions must be a non-empty array")
    lines = [
        "/* Generated backend execution facts.  Do not edit. */",
        "/* Define POSTGAMMA_BACKEND_EXECUTION before including. */",
        "",
    ]
    seen: set[str] = set()
    for index, execution in enumerate(executions):
        if not isinstance(execution, dict):
            raise FactError(f"alignment.executions[{index}] must be an object")
        symbol = execution.get("symbol")
        disposition = execution.get("disposition")
        execution_class = execution.get("execution_class", "none")
        if not isinstance(symbol, str) or not symbol or symbol in seen:
            raise FactError(f"alignment.executions[{index}] has an invalid symbol")
        if disposition not in {"thread", "logical_alias", "forbidden"}:
            raise FactError(
                f"alignment.executions[{index}] has an invalid disposition"
            )
        if execution_class not in {"client", "dedicated", "dynamic", "none"}:
            raise FactError(
                f"alignment.executions[{index}] has an invalid execution class"
            )
        seen.add(symbol)
        lines.append(
            "POSTGAMMA_BACKEND_EXECUTION("
            f"{symbol}, POSTGAMMA_BACKEND_DISPOSITION_{disposition.upper()}, "
            f"POSTGAMMA_BACKEND_POLICY_CLASS_{execution_class.upper()})"
        )
    lines.append("")
    return "\n".join(lines)


def write_if_changed(path: Path, content: str) -> None:
    data = content.encode("utf-8")
    if path.is_file() and path.read_bytes() == data:
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_bytes(data)
    temporary.replace(path)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--alignment", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    try:
        facts = emit_facts(load_alignment(args.alignment))
    except FactError as exc:
        parser.error(str(exc))
    write_if_changed(args.output, facts)
    print(f"generated backend execution facts -> {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
