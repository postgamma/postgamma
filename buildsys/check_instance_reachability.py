#!/usr/bin/env python3
"""Prove every instance-owned mutable PostgreSQL object has one runtime slot."""

from __future__ import annotations

import argparse
from pathlib import Path
from typing import Any

from check_embedded_lifecycle import input_identity, write_json
from check_c_api_release import ReleaseCheckError, load_document
from check_multi_instance import validate_instance_state_facts


CATALOG_KIND = "postgamma.backend-mutable-state-catalog"
POLICY_KIND = "postgamma.backend-state-ownership"
RUNTIME_KIND = "postgamma.backend-state-runtime"
MULTI_KIND = "postgamma.multi-instance"
EVIDENCE_KIND = "postgamma.instance-reachability"


class InstanceReachabilityError(RuntimeError):
    """The instance-owned state closure is incomplete or stale."""


def unique_objects(
    entries: Any, description: str
) -> dict[str, dict[str, Any]]:
    if not isinstance(entries, list):
        raise InstanceReachabilityError(f"{description} must be an array")
    result: dict[str, dict[str, Any]] = {}
    for entry in entries:
        if not isinstance(entry, dict) or not isinstance(entry.get("id"), str):
            raise InstanceReachabilityError(f"{description} contains an invalid entry")
        identifier = entry["id"]
        if not identifier or identifier in result:
            raise InstanceReachabilityError(
                f"{description} contains duplicate identity {identifier!r}"
            )
        result[identifier] = entry
    return result


def validate_reachability(
    catalog: dict[str, Any],
    policy: dict[str, Any],
    runtime: dict[str, Any],
    multi: dict[str, Any],
) -> dict[str, Any]:
    if catalog.get("kind") != CATALOG_KIND:
        raise InstanceReachabilityError("backend-state catalog kind changed")
    if policy.get("kind") != POLICY_KIND:
        raise InstanceReachabilityError("backend-state policy kind changed")
    if runtime.get("kind") != RUNTIME_KIND:
        raise InstanceReachabilityError("backend-state runtime kind changed")
    if multi.get("kind") != MULTI_KIND or multi.get("status") != "pass":
        raise InstanceReachabilityError("dual-live instance evidence is not passing")

    candidates = unique_objects(catalog.get("candidates"), "state catalog")
    decisions = unique_objects(policy.get("decisions"), "state policy")
    missing_decisions = sorted(set(candidates) - set(decisions))
    stale_required = sorted(
        identifier
        for identifier, decision in decisions.items()
        if identifier not in candidates
        and decision.get("availability", "required") != "conditional"
    )
    if missing_decisions or stale_required:
        details: list[str] = []
        if missing_decisions:
            details.append("unreviewed " + ", ".join(missing_decisions))
        if stale_required:
            details.append("stale required " + ", ".join(stale_required))
        raise InstanceReachabilityError(
            "backend-state policy is not source-exact: " + "; ".join(details)
        )

    instance_decisions = {
        identifier: decision
        for identifier, decision in decisions.items()
        if decision.get("owner") == "instance"
    }
    required_ids = {
        identifier
        for identifier, decision in instance_decisions.items()
        if decision.get("availability", "required") != "conditional"
    }
    instance_candidates = {
        identifier: candidates[identifier]
        for identifier in instance_decisions
        if identifier in candidates
    }
    missing_required = sorted(required_ids - set(instance_candidates))
    if missing_required:
        raise InstanceReachabilityError(
            "required instance candidates are absent: " + ", ".join(missing_required)
        )

    runtime_facts = validate_instance_state_facts(runtime, policy)
    runtime_ids = set(runtime_facts["slot_ids"])
    if runtime_ids != set(instance_candidates):
        missing = sorted(set(instance_candidates) - runtime_ids)
        unexpected = sorted(runtime_ids - set(instance_candidates))
        raise InstanceReachabilityError(
            "instance runtime is not catalog-exact: "
            f"missing={missing}, unexpected={unexpected}"
        )

    marker = multi.get("marker")
    recorded = multi.get("instance_state")
    if not isinstance(marker, dict) or marker.get("instances_live_peak") != "2":
        raise InstanceReachabilityError("dual-live evidence did not overlap instances")
    if recorded != runtime_facts:
        raise InstanceReachabilityError(
            "dual-live evidence used a different instance-state layout"
        )

    objects: list[dict[str, Any]] = []
    translation_units: set[str] = set()
    total_uses = 0
    for identifier in sorted(instance_candidates):
        candidate = instance_candidates[identifier]
        units = candidate.get("translation_units")
        use_count = candidate.get("use_count")
        if (
            not isinstance(units, list)
            or not units
            or any(not isinstance(unit, str) or not unit for unit in units)
            or not isinstance(use_count, int)
            or isinstance(use_count, bool)
            or use_count <= 0
        ):
            raise InstanceReachabilityError(
                f"instance object {identifier} has incomplete reachability facts"
            )
        translation_units.update(units)
        total_uses += use_count
        objects.append(
            {
                "id": identifier,
                "name": candidate.get("name"),
                "definition_path": candidate.get("definition_path"),
                "translation_units": sorted(set(units)),
                "use_count": use_count,
            }
        )

    return {
        "catalog_candidates": len(candidates),
        "reviewed_decisions": len(decisions),
        "conditional_decisions": sum(
            decision.get("availability") == "conditional"
            for decision in decisions.values()
        ),
        "instance_slots": runtime_facts["slots"],
        "instance_bytes": runtime_facts["bytes"],
        "instance_use_count": total_uses,
        "translation_units": sorted(translation_units),
        "objects": objects,
        "source_exact": True,
        "runtime_exact": True,
        "dual_live_instances": 2,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--catalog", required=True, type=Path)
    parser.add_argument("--policy", required=True, type=Path)
    parser.add_argument("--runtime", required=True, type=Path)
    parser.add_argument("--multi-instance", required=True, type=Path)
    parser.add_argument("--input", action="append", default=[], type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()

    try:
        catalog_path = args.catalog.resolve(strict=True)
        policy_path = args.policy.resolve(strict=True)
        runtime_path = args.runtime.resolve(strict=True)
        multi_path = args.multi_instance.resolve(strict=True)
        extra_inputs = [path.resolve(strict=True) for path in args.input]
        catalog = load_document(catalog_path, CATALOG_KIND)
        policy = load_document(policy_path, POLICY_KIND)
        runtime = load_document(runtime_path, RUNTIME_KIND)
        multi = load_document(multi_path, MULTI_KIND)
        report = validate_reachability(catalog, policy, runtime, multi)
        write_json(
            args.output.resolve(),
            {
                "schema_version": 1,
                "kind": EVIDENCE_KIND,
                "status": "pass",
                "postgresql_major": 19,
                "reachability": report,
                "inputs": input_identity(
                    [
                        *extra_inputs,
                        catalog_path,
                        policy_path,
                        runtime_path,
                        multi_path,
                    ]
                ),
            },
        )
    except (OSError, ReleaseCheckError, InstanceReachabilityError, ValueError) as exc:
        parser.error(str(exc))
    print(
        "instance-state reachability: pass "
        "(source-exact policy, runtime-exact slots, dual-live proof)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
