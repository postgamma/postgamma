#!/usr/bin/env python3
"""Require reviewed threaded runtime handling for every PostgreSQL backend execution type."""

from __future__ import annotations

import argparse
import json
from collections import Counter
from pathlib import Path
from typing import Any


class PolicyError(ValueError):
    """The backend execution policy is malformed or incomplete."""


DISPOSITIONS = frozenset({"thread", "logical_alias", "forbidden"})
EXECUTION_CLASSES = frozenset({"client", "dedicated", "dynamic"})


def load_document(path: Path, kind: str) -> dict[str, Any]:
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise PolicyError(f"cannot load {path}: {exc}") from exc
    if not isinstance(document, dict) or document.get("schema_version") != 1:
        raise PolicyError(f"{path}: expected a schema_version 1 object")
    if document.get("kind") != kind:
        raise PolicyError(f"{path}: expected kind {kind}")
    return document


def compile_alignment(
    catalog: dict[str, Any], policy: dict[str, Any]
) -> dict[str, Any]:
    executions = catalog.get("executions")
    decisions = policy.get("executions")
    if not isinstance(executions, list) or not isinstance(decisions, list):
        raise PolicyError("catalog and policy executions must be arrays")
    catalog_by_symbol: dict[str, dict[str, Any]] = {}
    for execution in executions:
        if not isinstance(execution, dict) or not isinstance(execution.get("symbol"), str):
            raise PolicyError("catalog contains an invalid execution entry")
        catalog_by_symbol[execution["symbol"]] = execution

    policy_symbols: list[str] = []
    decision_by_symbol: dict[str, dict[str, Any]] = {}
    for index, decision in enumerate(decisions):
        label = f"policy.executions[{index}]"
        if not isinstance(decision, dict):
            raise PolicyError(f"{label} must be an object")
        unknown = sorted(
            set(decision)
            - {"symbol", "disposition", "execution_class", "rationale"}
        )
        if unknown:
            raise PolicyError(f"{label} has unknown fields: {', '.join(unknown)}")
        symbol = decision.get("symbol")
        disposition = decision.get("disposition")
        rationale = decision.get("rationale")
        execution_class = decision.get("execution_class")
        if not isinstance(symbol, str) or not symbol:
            raise PolicyError(f"{label}.symbol must be a non-empty string")
        if disposition not in DISPOSITIONS:
            raise PolicyError(
                f"{label}.disposition must be one of {', '.join(sorted(DISPOSITIONS))}"
            )
        if not isinstance(rationale, str) or not rationale:
            raise PolicyError(f"{label}.rationale must be a non-empty string")
        if disposition == "thread":
            if execution_class not in EXECUTION_CLASSES:
                raise PolicyError(
                    f"{label}.execution_class must be one of "
                    f"{', '.join(sorted(EXECUTION_CLASSES))} for threaded entries"
                )
        elif execution_class is not None:
            raise PolicyError(
                f"{label}.execution_class is only valid for threaded entries"
            )
        policy_symbols.append(symbol)
        decision_by_symbol[symbol] = decision

    duplicates = sorted(
        symbol for symbol, count in Counter(policy_symbols).items() if count > 1
    )
    if duplicates:
        raise PolicyError("duplicate policy symbols: " + ", ".join(duplicates))
    missing = sorted(set(catalog_by_symbol) - set(decision_by_symbol))
    stale = sorted(set(decision_by_symbol) - set(catalog_by_symbol))
    if missing or stale:
        details: list[str] = []
        if missing:
            details.append("missing decisions: " + ", ".join(missing))
        if stale:
            details.append("stale decisions: " + ", ".join(stale))
        raise PolicyError("; ".join(details))

    records: list[dict[str, Any]] = []
    counts: Counter[str] = Counter()
    for symbol in sorted(catalog_by_symbol):
        execution = catalog_by_symbol[symbol]
        decision = decision_by_symbol[symbol]
        disposition = decision["disposition"]
        if disposition == "thread" and execution.get("main_function") is None:
            raise PolicyError(
                f"{symbol} cannot be threaded because upstream declares no main function"
            )
        if disposition == "logical_alias" and execution.get("main_function") is not None:
            raise PolicyError(
                f"{symbol} has an upstream main function and cannot be a logical alias"
            )
        counts[disposition] += 1
        records.append({**execution, **decision})

    return {
        "schema_version": 1,
        "kind": "postgamma.backend-execution-policy-alignment",
        "summary": {
            "execution_count": len(records),
            "threaded_count": counts["thread"],
            "logical_alias_count": counts["logical_alias"],
            "forbidden_count": counts["forbidden"],
        },
        "executions": records,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--catalog", required=True, type=Path)
    parser.add_argument("--policy", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    try:
        alignment = compile_alignment(
            load_document(args.catalog, "postgamma.backend-execution-catalog"),
            load_document(args.policy, "postgamma.backend-execution-policy"),
        )
    except PolicyError as exc:
        parser.error(str(exc))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(
        json.dumps(alignment, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    summary = alignment["summary"]
    print(
        "backend execution policy: "
        f"{summary['threaded_count']} threaded, "
        f"{summary['logical_alias_count']} logical alias, "
        f"{summary['forbidden_count']} forbidden"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
