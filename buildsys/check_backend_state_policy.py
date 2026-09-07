#!/usr/bin/env python3
"""Validate the human ownership contract for PostgreSQL backend state."""

from __future__ import annotations

import argparse
import json
from collections import Counter
from pathlib import Path
from typing import Any

from state_policy import (
    align_exact_candidates,
    catalog_candidates as common_catalog_candidates,
    load_json as common_load_json,
    policy_decisions as common_policy_decisions,
)


CATALOG_KIND = "postgamma.backend-mutable-state-catalog"
POLICY_KIND = "postgamma.backend-state-ownership"
REVIEW_RULES_KIND = "postgamma.backend-state-review-rules"
GUC_INVENTORY_MODE = "scan"
OWNERS = ("immutable", "instance", "managed_guc", "role", "session")
VIRTUAL_OWNERS = ("instance", "role", "session")
AVAILABILITIES = ("required", "conditional")


class BackendStatePolicyError(ValueError):
    """The source-derived catalog and the reviewed policy disagree."""


def load_json(path: Path) -> dict[str, Any]:
    return common_load_json(path, BackendStatePolicyError)


def catalog_candidates(document: dict[str, Any]) -> dict[str, dict[str, Any]]:
    return common_catalog_candidates(document, BackendStatePolicyError)


def policy_decisions(document: dict[str, Any]) -> dict[str, dict[str, Any]]:
    return common_policy_decisions(
        document,
        policy_kind=POLICY_KIND,
        owners=OWNERS,
        error_type=BackendStatePolicyError,
    )


def review_rules(document: dict[str, Any]) -> dict[str, Any]:
    if (
        document.get("schema_version") != 1
        or document.get("kind") != REVIEW_RULES_KIND
    ):
        raise BackendStatePolicyError(
            f"review rules: expected schema_version 1 and kind {REVIEW_RULES_KIND}"
        )
    default = document.get("default_decision")
    if not isinstance(default, dict):
        raise BackendStatePolicyError("review rules: default_decision must be an object")
    if default.get("owner") != "role" or not isinstance(default.get("rationale"), str) or not default["rationale"]:
        raise BackendStatePolicyError(
            "review rules: default_decision must define a role owner and non-empty rationale"
        )
    raw = document.get("rules")
    if not isinstance(raw, list):
        raise BackendStatePolicyError("review rules: rules must be an array")
    result: dict[str, dict[str, str]] = {}
    allowed_fields = {"id", "owner", "rationale", "availability", "condition"}
    for index, rule in enumerate(raw):
        if not isinstance(rule, dict):
            raise BackendStatePolicyError(f"review rules: rules[{index}] must be an object")
        unknown = sorted(set(rule) - allowed_fields)
        if unknown:
            raise BackendStatePolicyError(
                f"review rules: rules[{index}] has unknown field(s): " + ", ".join(unknown)
            )
        identifier = rule.get("id")
        if not isinstance(identifier, str) or not identifier:
            raise BackendStatePolicyError(f"review rules: rules[{index}] has invalid id")
        if identifier in result:
            raise BackendStatePolicyError(f"review rules: duplicate rule {identifier}")
        owner = rule.get("owner")
        rationale = rule.get("rationale")
        availability = rule.get("availability")
        condition = rule.get("condition")
        if owner is not None:
            if owner not in OWNERS or owner == "managed_guc":
                raise BackendStatePolicyError(
                    f"review rules: {identifier} has invalid explicit owner {owner!r}"
                )
            if not isinstance(rationale, str) or not rationale:
                raise BackendStatePolicyError(
                    f"review rules: {identifier} requires a rationale for explicit owner"
                )
        elif rationale is not None:
            raise BackendStatePolicyError(
                f"review rules: {identifier} has a rationale without an explicit owner"
            )
        if availability is not None:
            if availability != "conditional":
                raise BackendStatePolicyError(
                    f"review rules: {identifier} may only declare conditional availability"
                )
            if not isinstance(condition, str) or not condition:
                raise BackendStatePolicyError(
                    f"review rules: {identifier} requires a non-empty condition"
                )
        elif condition is not None:
            raise BackendStatePolicyError(
                f"review rules: {identifier} has a condition without conditional availability"
            )
        if owner is None and availability is None:
            raise BackendStatePolicyError(
                f"review rules: {identifier} does not define an ownership or availability exception"
            )
        result[identifier] = dict(rule)
    return {"default_decision": dict(default), "rules": result}


def validate_review_rule_alignment(
    policy: dict[str, Any], rules_document: dict[str, Any]
) -> dict[str, Any]:
    decisions = policy_decisions(policy)
    parsed = review_rules(rules_document)
    rules = parsed["rules"]
    stale = sorted(set(rules) - set(decisions))
    if stale:
        raise BackendStatePolicyError(
            "review rules: stale rule(s): " + ", ".join(stale)
        )
    problems: list[str] = []
    for identifier, rule in sorted(rules.items()):
        decision = decisions[identifier]
        for field in ("owner", "rationale", "availability", "condition"):
            if field in rule and decision.get(field) != rule[field]:
                problems.append(
                    f"{identifier} {field} is {decision.get(field)!r}, expected {rule[field]!r}"
                )
    missing_owner_rules = sorted(
        identifier
        for identifier, decision in decisions.items()
        if decision["owner"] not in ("role", "managed_guc")
        and (identifier not in rules or "owner" not in rules[identifier])
    )
    missing_availability_rules = sorted(
        identifier
        for identifier, decision in decisions.items()
        if decision["availability"] == "conditional"
        and (identifier not in rules or "availability" not in rules[identifier])
    )
    if missing_owner_rules:
        problems.append(
            "non-default ownership missing review rule: " + ", ".join(missing_owner_rules)
        )
    if missing_availability_rules:
        problems.append(
            "conditional availability missing review rule: "
            + ", ".join(missing_availability_rules)
        )
    if problems:
        raise BackendStatePolicyError("review rules: " + "; ".join(problems))
    return {
        "rule_count": len(rules),
        "explicit_owner_rule_count": sum("owner" in rule for rule in rules.values()),
        "conditional_rule_count": sum(
            "availability" in rule for rule in rules.values()
        ),
    }


def guc_definition_usrs(document: dict[str, Any]) -> set[str]:
    if document.get("schema_version") != 1 or document.get("mode") != GUC_INVENTORY_MODE:
        raise BackendStatePolicyError("GUC inventory: expected schema_version 1 scan output")
    raw = document.get("declarations")
    if not isinstance(raw, list):
        raise BackendStatePolicyError("GUC inventory: declarations must be an array")
    result = {
        declaration["usr"]
        for declaration in raw
        if isinstance(declaration, dict)
        and declaration.get("definition") is True
        and isinstance(declaration.get("usr"), str)
        and declaration["usr"]
        and isinstance(declaration.get("symbol_id"), str)
        and declaration["symbol_id"].startswith("pg.guc.")
    }
    if not result:
        raise BackendStatePolicyError("GUC inventory: no built-in definitions found")
    return result


def validate_alignment(
    catalog: dict[str, Any],
    policy: dict[str, Any],
    guc_inventory: dict[str, Any],
    rules_document: dict[str, Any] | None = None,
) -> dict[str, Any]:
    candidates = catalog_candidates(catalog)
    decisions = policy_decisions(policy)
    candidate_ids = set(candidates)
    dormant = align_exact_candidates(
        candidates, decisions, BackendStatePolicyError
    )

    guc_usrs = guc_definition_usrs(guc_inventory)
    managed_ids = {identifier for identifier, value in candidates.items() if value["usr"] in guc_usrs}
    policy_managed_ids = {
        identifier
        for identifier, decision in decisions.items()
        if identifier in candidate_ids and decision["owner"] == "managed_guc"
    }
    if managed_ids != policy_managed_ids:
        missing_managed = sorted(managed_ids - policy_managed_ids)
        false_managed = sorted(policy_managed_ids - managed_ids)
        details = []
        if missing_managed:
            details.append("GUC-backed candidates not marked managed_guc: " + ", ".join(missing_managed))
        if false_managed:
            details.append("non-GUC candidates marked managed_guc: " + ", ".join(false_managed))
        raise BackendStatePolicyError("; ".join(details))

    for identifier in sorted(candidate_ids):
        decision = decisions[identifier]
        if decision["owner"] not in VIRTUAL_OWNERS:
            continue
        candidate = candidates[identifier]
        if not candidate.get("complete_type") or not candidate.get("trivially_copyable"):
            raise BackendStatePolicyError(
                f"{identifier}: virtual state requires a complete, trivially-copyable type"
            )
        if not isinstance(candidate.get("size"), int) or candidate["size"] <= 0:
            raise BackendStatePolicyError(f"{identifier}: invalid virtual-state size")
        if not isinstance(candidate.get("alignment"), int) or candidate["alignment"] <= 0:
            raise BackendStatePolicyError(f"{identifier}: invalid virtual-state alignment")

    relocations: set[tuple[str, str]] = set()
    for index, use in enumerate(catalog.get("uses", [])):
        if not isinstance(use, dict):
            raise BackendStatePolicyError(f"catalog: uses[{index}] must be an object")
        target_id = use.get("id")
        if target_id not in decisions or decisions[target_id]["owner"] not in VIRTUAL_OWNERS:
            continue
        if not use.get("in_static_initializer"):
            continue
        owner_id = use.get("static_initializer_owner_id")
        if owner_id not in decisions:
            raise BackendStatePolicyError(
                f"{target_id}: address is embedded in non-virtual static initializer {owner_id!r}"
            )
        target_owner = decisions[target_id]["owner"]
        initializer_owner = decisions[owner_id]["owner"]
        if initializer_owner != target_owner:
            raise BackendStatePolicyError(
                f"{target_id}: static initializer owner {owner_id} has incompatible owner "
                f"{initializer_owner} (expected {target_owner})"
            )
        relocations.add((owner_id, target_id))

    counts = Counter(decisions[identifier]["owner"] for identifier in candidate_ids)
    policy_counts = Counter(decision["owner"] for decision in decisions.values())
    transformed = sum(counts[owner] for owner in VIRTUAL_OWNERS)
    report = {
        "schema_version": 1,
        "kind": "postgamma.backend-state-ownership-alignment",
        "candidate_count": len(candidates),
        "ownership_counts": {owner: counts[owner] for owner in OWNERS},
        "policy_ownership_counts": {owner: policy_counts[owner] for owner in OWNERS},
        "transformed_candidate_count": transformed,
        "guc_managed_candidate_count": len(managed_ids),
        "relocations": [
            {"owner_id": owner_id, "target_id": target_id}
            for owner_id, target_id in sorted(relocations)
        ],
        "skipped_constant_initializer_use_count": 0,
        "missing": [],
        "stale": [],
        "dormant_conditional_count": len(dormant),
        "dormant_conditional_decisions": dormant,
    }
    if rules_document is not None:
        report["review_rules"] = validate_review_rule_alignment(policy, rules_document)
    return report


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--catalog", required=True, type=Path)
    parser.add_argument("--policy", required=True, type=Path)
    parser.add_argument("--guc-inventory", required=True, type=Path)
    parser.add_argument("--review-rules", required=True, type=Path)
    parser.add_argument("--report", required=True, type=Path)
    args = parser.parse_args()
    try:
        report = validate_alignment(
            load_json(args.catalog),
            load_json(args.policy),
            load_json(args.guc_inventory),
            load_json(args.review_rules),
        )
    except BackendStatePolicyError as exc:
        parser.error(str(exc))
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(
        json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    counts = report["ownership_counts"]
    print(
        f"backend state policy: {report['candidate_count']} exact decisions; "
        + ", ".join(f"{owner}={counts[owner]}" for owner in OWNERS)
        + f"; relocations={len(report['relocations'])}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
