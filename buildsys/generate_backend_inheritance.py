#!/usr/bin/env python3
"""Validate backend inheritance policy and emit data-only C facts."""

from __future__ import annotations

import argparse
import json
from collections import Counter
from pathlib import Path
from typing import Any


CATALOG_KIND = "postgamma.backend-inheritance-catalog"
POLICY_KIND = "postgamma.backend-inheritance-policy"
RUNTIME_KIND = "postgamma.backend-state-runtime"
ALIGNMENT_KIND = "postgamma.backend-inheritance-alignment"
STRATEGIES = frozenset({"copy_value", "borrow_instance"})


class InheritanceError(ValueError):
    """The backend inheritance contract is malformed or incomplete."""


def load_document(path: Path, kind: str) -> dict[str, Any]:
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise InheritanceError(f"cannot load {path}: {exc}") from exc
    if not isinstance(document, dict) or document.get("schema_version") != 1:
        raise InheritanceError(f"{path}: expected a schema_version 1 object")
    if document.get("kind") != kind:
        raise InheritanceError(f"{path}: expected kind {kind}")
    return document


def _entries_by_id(entries: Any, label: str) -> dict[str, dict[str, Any]]:
    if not isinstance(entries, list):
        raise InheritanceError(f"{label} must be an array")
    identifiers: list[str] = []
    result: dict[str, dict[str, Any]] = {}
    for index, entry in enumerate(entries):
        item_label = f"{label}[{index}]"
        if not isinstance(entry, dict):
            raise InheritanceError(f"{item_label} must be an object")
        identifier = entry.get("id")
        if not isinstance(identifier, str) or not identifier:
            raise InheritanceError(f"{item_label}.id must be a non-empty string")
        identifiers.append(identifier)
        result[identifier] = entry
    duplicates = sorted(
        identifier
        for identifier, count in Counter(identifiers).items()
        if count > 1
    )
    if duplicates:
        raise InheritanceError(f"{label} contains duplicate ids: {', '.join(duplicates)}")
    return result


def compile_alignment(
    catalog: dict[str, Any],
    policy: dict[str, Any],
    runtime: dict[str, Any],
) -> dict[str, Any]:
    for field, expected in (
        ("guc_transfer", "postgresql_serialize_restore"),
        ("startup_data_transfer", "copy_bytes"),
        ("client_socket_transfer", "move_descriptor_ownership"),
    ):
        if policy.get(field) != expected:
            raise InheritanceError(f"policy.{field} must be {expected}")

    catalog_by_id = _entries_by_id(catalog.get("states"), "catalog.states")
    policy_by_id = _entries_by_id(policy.get("states"), "policy.states")
    runtime_by_id = _entries_by_id(runtime.get("slots"), "runtime.slots")
    runtime_by_name: dict[str, list[dict[str, Any]]] = {}
    for state in runtime_by_id.values():
        name = state.get("name")
        if isinstance(name, str):
            runtime_by_name.setdefault(name, []).append(state)

    allowed_policy_fields = {
        "id",
        "strategy",
        "rationale",
        "availability",
        "condition",
    }
    for identifier, decision in policy_by_id.items():
        unknown = sorted(set(decision) - allowed_policy_fields)
        if unknown:
            raise InheritanceError(
                f"policy state {identifier} has unknown fields: {', '.join(unknown)}"
            )
        if decision.get("strategy") not in STRATEGIES:
            raise InheritanceError(
                f"policy state {identifier}.strategy must be one of "
                f"{', '.join(sorted(STRATEGIES))}"
            )
        rationale = decision.get("rationale")
        if not isinstance(rationale, str) or not rationale:
            raise InheritanceError(
                f"policy state {identifier}.rationale must be a non-empty string"
            )
        availability = decision.get("availability")
        condition = decision.get("condition")
        if availability is None and condition is not None:
            raise InheritanceError(
                f"policy state {identifier}.condition requires conditional availability"
            )
        if availability is not None:
            if availability != "conditional" or not isinstance(condition, str) or not condition:
                raise InheritanceError(
                    f"policy state {identifier} has invalid conditional availability"
                )

    missing = sorted(set(catalog_by_id) - set(policy_by_id))
    stale = sorted(
        identifier
        for identifier in set(policy_by_id) - set(catalog_by_id)
        if policy_by_id[identifier].get("availability") != "conditional"
    )
    if missing or stale:
        details: list[str] = []
        if missing:
            details.append("missing decisions: " + ", ".join(missing))
        if stale:
            details.append("stale decisions: " + ", ".join(stale))
        raise InheritanceError("; ".join(details))

    states: list[dict[str, Any]] = []
    strategy_counts: Counter[str] = Counter()
    for contract_identifier in sorted(catalog_by_id):
        contract_state = catalog_by_id[contract_identifier]
        state = runtime_by_id.get(contract_identifier)
        if state is None:
            name = contract_state.get("name")
            canonical_type = contract_state.get("canonical_type")
            candidates = [
                candidate
                for candidate in runtime_by_name.get(name, [])
                if candidate.get("canonical_type") == canonical_type
            ]
            if len(candidates) != 1:
                raise InheritanceError(
                    f"inherited state {contract_identifier} cannot bind uniquely to "
                    "the normal-build backend-state runtime"
                )
            state = candidates[0]
        identifier = state["id"]
        if state.get("owner") != "role":
            raise InheritanceError(
                f"inherited state {contract_identifier} must be role-owned, "
                f"found {state.get('owner')}"
            )
        relocations = state.get("relocations")
        if not isinstance(relocations, list) or relocations:
            raise InheritanceError(
                f"inherited state {contract_identifier} must not require pointer relocation"
            )
        decision = policy_by_id[contract_identifier]
        strategy = decision["strategy"]
        strategy_counts[strategy] += 1
        states.append(
            {
                "id": identifier,
                "contract_id": contract_identifier,
                "name": contract_state.get("name"),
                "canonical_type": contract_state.get("canonical_type"),
                "strategy": strategy,
                "rationale": decision["rationale"],
                "slot": state.get("enum"),
                "size": state.get("size"),
                "alignment": state.get("alignment"),
            }
        )

    contract = catalog.get("contract")
    if not isinstance(contract, dict):
        raise InheritanceError("catalog.contract must be an object")
    return {
        "schema_version": 1,
        "kind": ALIGNMENT_KIND,
        "contract": contract,
        "transfers": {
            "guc": policy["guc_transfer"],
            "startup_data": policy["startup_data_transfer"],
            "client_socket": policy["client_socket_transfer"],
        },
        "summary": {
            "state_count": len(states),
            "copy_value_count": strategy_counts["copy_value"],
            "borrow_instance_count": strategy_counts["borrow_instance"],
            "dormant_conditional_count": sum(
                identifier not in catalog_by_id
                and decision.get("availability") == "conditional"
                for identifier, decision in policy_by_id.items()
            ),
        },
        "states": states,
    }


def render_facts(alignment: dict[str, Any]) -> str:
    lines = [
        "/* Generated backend inheritance facts.  Do not edit. */",
        "/* Define POSTGAMMA_BACKEND_INHERITED_STATE before including. */",
        "",
    ]
    for state in alignment["states"]:
        strategy = str(state["strategy"]).upper()
        lines.append(
            f'POSTGAMMA_BACKEND_INHERITED_STATE("{state["id"]}", '
            f"POSTGAMMA_BACKEND_INHERIT_{strategy})"
        )
    lines.append("")
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--catalog", required=True, type=Path)
    parser.add_argument("--policy", required=True, type=Path)
    parser.add_argument("--runtime", required=True, type=Path)
    parser.add_argument("--report", required=True, type=Path)
    parser.add_argument("--facts", required=True, type=Path)
    args = parser.parse_args()
    try:
        alignment = compile_alignment(
            load_document(args.catalog, CATALOG_KIND),
            load_document(args.policy, POLICY_KIND),
            load_document(args.runtime, RUNTIME_KIND),
        )
    except InheritanceError as exc:
        parser.error(str(exc))
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.facts.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(
        json.dumps(alignment, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    args.facts.write_text(render_facts(alignment), encoding="utf-8")
    summary = alignment["summary"]
    print(
        "backend inheritance: "
        f"{summary['state_count']} states "
        f"({summary['copy_value_count']} copied, "
        f"{summary['borrow_instance_count']} borrowed)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
