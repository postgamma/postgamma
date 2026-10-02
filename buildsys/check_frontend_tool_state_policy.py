#!/usr/bin/env python3
"""Validate PostgreSQL frontend/tool state ownership and bootstrap reachability."""

from __future__ import annotations

import argparse
import json
from collections import Counter
from pathlib import Path
from typing import Any

from state_policy import (
    align_exact_candidates,
    catalog_candidates,
    load_json,
    policy_decisions as common_policy_decisions,
    reject_unknown,
)


POLICY_KIND = "postgamma.frontend-tool-state-ownership"
REACHABILITY_KIND = "postgamma.frontend-tool-state-reachability"
REPORT_KIND = "postgamma.frontend-tool-state-ownership-alignment"
OWNERS = (
    "library_immutable",
    "library_synchronized",
    "tool",
    "connection",
    "forbidden",
)
CONTEXT_OWNERS = ("tool", "connection")


class FrontendToolStatePolicyError(ValueError):
    """The frontend/tool catalog, policy, or bootstrap slice is inconsistent."""


def policy_decisions(document: dict[str, Any]) -> dict[str, dict[str, Any]]:
    decisions = common_policy_decisions(
        document,
        policy_kind=POLICY_KIND,
        owners=OWNERS,
        error_type=FrontendToolStatePolicyError,
        allowed_decision_fields=(
            "id",
            "owner",
            "rationale",
            "availability",
            "condition",
            "synchronization",
            "replacement",
        ),
    )
    for identifier, decision in decisions.items():
        synchronization = decision.get("synchronization")
        replacement = decision.get("replacement")
        if decision["owner"] == "library_synchronized":
            if not isinstance(synchronization, str) or not synchronization:
                raise FrontendToolStatePolicyError(
                    f"policy: {identifier} requires a synchronization contract"
                )
        elif synchronization is not None:
            raise FrontendToolStatePolicyError(
                f"policy: {identifier} has synchronization but is not library_synchronized"
            )
        if decision["owner"] == "forbidden":
            if not isinstance(replacement, str) or not replacement:
                raise FrontendToolStatePolicyError(
                    f"policy: {identifier} requires a replacement contract"
                )
        elif replacement is not None:
            raise FrontendToolStatePolicyError(
                f"policy: {identifier} has replacement but is not forbidden"
            )
    return decisions


def _uses_by_candidate(catalog: dict[str, Any]) -> dict[str, list[dict[str, Any]]]:
    result: dict[str, list[dict[str, Any]]] = {}
    for index, use in enumerate(catalog.get("uses", [])):
        if not isinstance(use, dict):
            raise FrontendToolStatePolicyError(
                f"catalog: uses[{index}] must be an object"
            )
        identifier = use.get("id")
        if not isinstance(identifier, str) or not identifier:
            raise FrontendToolStatePolicyError(
                f"catalog: uses[{index}] has an invalid id"
            )
        result.setdefault(identifier, []).append(use)
    return result


def validate_reachability(
    document: dict[str, Any], candidates: dict[str, dict[str, Any]]
) -> dict[str, list[str]]:
    reject_unknown(
        document,
        {"schema_version", "kind", "slices"},
        "reachability",
        FrontendToolStatePolicyError,
    )
    if (
        document.get("schema_version") != 1
        or document.get("kind") != REACHABILITY_KIND
    ):
        raise FrontendToolStatePolicyError(
            f"reachability: expected schema_version 1 and kind {REACHABILITY_KIND}"
        )
    raw_slices = document.get("slices")
    if not isinstance(raw_slices, list) or not raw_slices:
        raise FrontendToolStatePolicyError(
            "reachability: slices must be a non-empty array"
        )
    result: dict[str, list[str]] = {}
    for index, raw in enumerate(raw_slices):
        label = f"reachability: slices[{index}]"
        if not isinstance(raw, dict):
            raise FrontendToolStatePolicyError(f"{label} must be an object")
        reject_unknown(
            raw,
            {"id", "candidate_ids", "provenance"},
            label,
            FrontendToolStatePolicyError,
        )
        identifier = raw.get("id")
        candidate_ids = raw.get("candidate_ids")
        provenance = raw.get("provenance")
        if not isinstance(identifier, str) or not identifier:
            raise FrontendToolStatePolicyError(f"{label}.id must be non-empty")
        if identifier in result:
            raise FrontendToolStatePolicyError(
                f"reachability: duplicate slice {identifier}"
            )
        if not isinstance(candidate_ids, list) or any(
            not isinstance(value, str) or not value for value in candidate_ids
        ):
            raise FrontendToolStatePolicyError(
                f"{label}.candidate_ids must be an array of non-empty strings"
            )
        if candidate_ids != sorted(set(candidate_ids)):
            raise FrontendToolStatePolicyError(
                f"{label}.candidate_ids must be sorted and unique"
            )
        if not isinstance(provenance, str) or not provenance:
            raise FrontendToolStatePolicyError(
                f"{label}.provenance must be non-empty"
            )
        unknown = sorted(set(candidate_ids) - set(candidates))
        if unknown:
            raise FrontendToolStatePolicyError(
                f"reachability: {identifier} references unknown candidate(s): "
                + ", ".join(unknown)
            )
        result[identifier] = candidate_ids
    return result


def validate_alignment(
    catalog: dict[str, Any],
    policy: dict[str, Any],
    reachability: dict[str, Any] | None = None,
) -> dict[str, Any]:
    candidates = catalog_candidates(catalog, FrontendToolStatePolicyError)
    decisions = policy_decisions(policy)
    dormant = align_exact_candidates(
        candidates, decisions, FrontendToolStatePolicyError
    )
    uses_by_candidate = _uses_by_candidate(catalog)

    immutable_writes: list[str] = []
    relocations: set[tuple[str, str]] = set()
    for identifier in sorted(candidates):
        decision = decisions[identifier]
        if decision["owner"] == "library_immutable" and any(
            use.get("use_kind") in ("write", "read_write")
            for use in uses_by_candidate.get(identifier, [])
        ):
            immutable_writes.append(identifier)
    if immutable_writes:
        raise FrontendToolStatePolicyError(
            "library_immutable state has runtime write(s): "
            + ", ".join(immutable_writes)
        )

    for use in catalog.get("uses", []):
        target_id = use["id"]
        if (
            target_id not in decisions
            or decisions[target_id]["owner"] not in CONTEXT_OWNERS
        ):
            continue
        if not use.get("in_static_initializer"):
            continue
        owner_id = use.get("static_initializer_owner_id")
        if owner_id not in decisions:
            raise FrontendToolStatePolicyError(
                f"{target_id}: address is embedded in unowned static initializer {owner_id!r}"
            )
        target_owner = decisions[target_id]["owner"]
        initializer_owner = decisions[owner_id]["owner"]
        if initializer_owner != target_owner:
            raise FrontendToolStatePolicyError(
                f"{target_id}: static initializer owner {owner_id} has incompatible owner "
                f"{initializer_owner} (expected {target_owner})"
            )
        relocations.add((owner_id, target_id))

    counts = Counter(decisions[identifier]["owner"] for identifier in candidates)
    slice_report: dict[str, Any] = {}
    if reachability is not None:
        slices = validate_reachability(reachability, candidates)
        blocked: list[str] = []
        for slice_id, candidate_ids in sorted(slices.items()):
            forbidden = sorted(
                identifier
                for identifier in candidate_ids
                if decisions[identifier]["owner"] == "forbidden"
            )
            if forbidden:
                blocked.append(slice_id + ": " + ", ".join(forbidden))
            slice_report[slice_id] = {
                "candidate_count": len(candidate_ids),
                "forbidden": forbidden,
                "ownership_counts": dict(
                    sorted(
                        Counter(
                            decisions[value]["owner"] for value in candidate_ids
                        ).items()
                    )
                ),
            }
        if blocked:
            raise FrontendToolStatePolicyError(
                "bootstrap slice reaches forbidden state: " + "; ".join(blocked)
            )

    return {
        "schema_version": 1,
        "kind": REPORT_KIND,
        "candidate_count": len(candidates),
        "ownership_counts": {owner: counts[owner] for owner in OWNERS},
        "context_transformed_candidate_count": sum(
            counts[owner] for owner in CONTEXT_OWNERS
        ),
        "relocations": [
            {"owner_id": owner_id, "target_id": target_id}
            for owner_id, target_id in sorted(relocations)
        ],
        "dormant_conditional_count": len(dormant),
        "dormant_conditional_decisions": dormant,
        "bootstrap_slices": slice_report,
        "missing": [],
        "stale": [],
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--catalog", required=True, type=Path)
    parser.add_argument("--policy", required=True, type=Path)
    parser.add_argument("--reachability", type=Path)
    parser.add_argument("--report", required=True, type=Path)
    args = parser.parse_args()
    try:
        report = validate_alignment(
            load_json(args.catalog, FrontendToolStatePolicyError),
            load_json(args.policy, FrontendToolStatePolicyError),
            (
                load_json(args.reachability, FrontendToolStatePolicyError)
                if args.reachability
                else None
            ),
        )
    except FrontendToolStatePolicyError as exc:
        parser.error(str(exc))
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(
        json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    counts = report["ownership_counts"]
    print(
        f"frontend/tool state policy: {report['candidate_count']} exact decisions; "
        + ", ".join(f"{owner}={counts[owner]}" for owner in OWNERS)
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
