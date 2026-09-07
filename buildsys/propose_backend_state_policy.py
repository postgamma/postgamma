#!/usr/bin/env python3
"""Create a reviewable first ownership policy from source and oracle facts.

This is deliberately not a normal build target.  It bootstraps a policy when
adopting a PostgreSQL baseline; the checked-in result is then reviewed and is
validated exactly against every subsequent AST inventory.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import Any

from check_backend_state_policy import (
    BackendStatePolicyError,
    catalog_candidates,
    load_json,
    policy_decisions,
    review_rules,
)


ORACLE_KIND = "postgamma.threaded-oracle-state-coverage"
POLICY_KIND = "postgamma.backend-state-ownership"

def apply_review_rule(
    decision: dict[str, Any], rule: dict[str, Any] | None, managed_guc: bool
) -> dict[str, Any]:
    result = dict(decision)
    if rule is not None and "owner" in rule:
        if managed_guc:
            raise BackendStatePolicyError(
                f"{result['id']}: reviewed owner {rule['owner']} conflicts with GUC ownership"
            )
        result["owner"] = rule["owner"]
        result["rationale"] = rule["rationale"]
    if rule is None or "availability" not in rule:
        result.pop("availability", None)
        result.pop("condition", None)
    else:
        result["availability"] = rule["availability"]
        result["condition"] = rule["condition"]
    return result


def oracle_ids(document: dict[str, Any]) -> set[str]:
    if document.get("schema_version") != 1 or document.get("kind") != ORACLE_KIND:
        raise ValueError(f"oracle: expected schema_version 1 and kind {ORACLE_KIND}")
    matches = document.get("matches")
    if not isinstance(matches, list):
        raise ValueError("oracle: matches must be an array")
    result: set[str] = set()
    for index, match in enumerate(matches):
        if not isinstance(match, dict) or not isinstance(match.get("id"), str):
            raise ValueError(f"oracle: matches[{index}] is invalid")
        result.add(match["id"])
    return result


def guc_usrs(document: dict[str, Any]) -> set[str]:
    declarations = document.get("declarations")
    if document.get("schema_version") != 1 or not isinstance(declarations, list):
        raise ValueError("GUC inventory: invalid scan output")
    return {
        declaration["usr"]
        for declaration in declarations
        if isinstance(declaration, dict)
        and declaration.get("definition") is True
        and isinstance(declaration.get("usr"), str)
        and isinstance(declaration.get("symbol_id"), str)
        and declaration["symbol_id"].startswith("pg.guc.")
    }


def propose(
    catalog: dict[str, Any],
    guc_inventory: dict[str, Any],
    oracle: dict[str, Any] | None,
    rules_document: dict[str, Any],
    existing_policy: dict[str, Any] | None = None,
) -> dict[str, Any]:
    candidates = catalog_candidates(catalog)
    managed_usrs = guc_usrs(guc_inventory)
    historical = oracle_ids(oracle) if oracle is not None else set()
    existing = policy_decisions(existing_policy) if existing_policy is not None else {}
    parsed_rules = review_rules(rules_document)
    default = parsed_rules["default_decision"]
    rules = parsed_rules["rules"]
    decisions = []
    for identifier, candidate in sorted(candidates.items()):
        managed_guc = candidate["usr"] in managed_usrs
        if managed_guc:
            decision = {
                "id": identifier,
                "owner": "managed_guc",
                "rationale": "owned by the exhaustive built-in GUC state pipeline",
            }
        elif identifier in existing:
            decision = existing[identifier]
        else:
            decision = {
                "id": identifier,
                "owner": default["owner"],
                "rationale": default["rationale"],
            }
            if identifier in historical:
                decision["rationale"] = (
                    "confirmed role-local by the reviewed threaded PostgreSQL oracle"
                )
        decisions.append(
            apply_review_rule(decision, rules.get(identifier), managed_guc)
        )
    for identifier in sorted(set(existing) - set(candidates)):
        decisions.append(
            apply_review_rule(existing[identifier], rules.get(identifier), False)
        )
    decisions.sort(key=lambda decision: decision["id"])
    return {
        "schema_version": 1,
        "kind": POLICY_KIND,
        "policy": (
            "Every mutable backend object is role-local unless another reviewed owner "
            "is explicit. PostgreSQL shared-memory payloads remain shared; their "
            "process-local attachment variables follow the role, matching fork semantics."
        ),
        "decisions": decisions,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--catalog", required=True, type=Path)
    parser.add_argument("--guc-inventory", required=True, type=Path)
    parser.add_argument("--oracle-report", type=Path)
    parser.add_argument("--review-rules", required=True, type=Path)
    parser.add_argument("--existing-policy", type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    try:
        document = propose(
            load_json(args.catalog),
            load_json(args.guc_inventory),
            load_json(args.oracle_report) if args.oracle_report else None,
            load_json(args.review_rules),
            load_json(args.existing_policy) if args.existing_policy else None,
        )
    except (ValueError, KeyError, TypeError) as exc:
        parser.error(str(exc))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(
        json.dumps(document, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    print(f"proposed {len(document['decisions'])} backend ownership decisions")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
