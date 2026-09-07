#!/usr/bin/env python3
"""Compare two PostgreSQL-derived fact sets and emit a semantic upgrade report."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import Any, Iterable

from check_backend_state_policy import (
    BackendStatePolicyError,
    catalog_candidates,
    guc_definition_usrs,
    policy_decisions,
)
from check_guc_policy import CATALOG_KIND as GUC_CATALOG_KIND
from postgresql_adapter import AdapterError, adapter_sha256, load_adapter


REPORT_KIND = "postgamma.postgresql-upgrade-audit"
REPORT_SCHEMA_VERSION = 2
STATE_STRUCTURAL_FIELDS = (
    "name",
    "canonical_type",
    "size",
    "alignment",
    "array",
    "atomic",
    "volatile",
    "complete_type",
    "trivially_copyable",
    "externally_visible",
    "file_scope",
    "function_static",
    "storage_class",
    "tls_kind",
    "definition_kind",
    "definition_path",
    "definition_source_kind",
    "has_initializer",
    "constant_initializer",
    "initializer_class",
)
STATE_IDENTITY_METADATA_FIELDS = ("usr",)
STATE_TOPOLOGY_FIELDS = (
    "declaration_paths",
    "translation_units",
    "use_count",
    "macro_use_count",
    "static_initializer_use_count",
    "uses_by_kind",
)
MOVE_FINGERPRINT_FIELDS = (
    "name",
    "canonical_type",
    "size",
    "alignment",
    "externally_visible",
    "file_scope",
    "function_static",
    "storage_class",
    "initializer_class",
)


class UpgradeAuditError(ValueError):
    """An upgrade input is malformed or internally inconsistent."""


def load_json(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise UpgradeAuditError(f"cannot load {path}: {exc}") from exc
    if not isinstance(value, dict):
        raise UpgradeAuditError(f"{path}: top-level value must be an object")
    return value


def _field_changes(
    baseline: dict[str, Any], candidate: dict[str, Any], fields: Iterable[str]
) -> dict[str, dict[str, Any]]:
    result: dict[str, dict[str, Any]] = {}
    for field in fields:
        if baseline.get(field) != candidate.get(field):
            result[field] = {
                "baseline": baseline.get(field),
                "candidate": candidate.get(field),
            }
    return result


def guc_parameters(document: dict[str, Any]) -> dict[str, dict[str, Any]]:
    if (
        document.get("schema_version") != 1
        or document.get("kind") != GUC_CATALOG_KIND
    ):
        raise UpgradeAuditError(
            f"GUC catalog must use schema_version 1 and kind {GUC_CATALOG_KIND}"
        )
    raw = document.get("parameters")
    if not isinstance(raw, list):
        raise UpgradeAuditError("GUC catalog parameters must be an array")
    result: dict[str, dict[str, Any]] = {}
    for index, parameter in enumerate(raw):
        if not isinstance(parameter, dict):
            raise UpgradeAuditError(f"GUC parameters[{index}] must be an object")
        name = parameter.get("name")
        if not isinstance(name, str) or not name:
            raise UpgradeAuditError(f"GUC parameters[{index}] has invalid name")
        if name in result:
            raise UpgradeAuditError(f"duplicate GUC parameter {name}")
        result[name] = parameter
    if document.get("parameter_count") != len(result):
        raise UpgradeAuditError("GUC catalog parameter_count does not match parameters")
    return result


def diff_gucs(
    baseline_document: dict[str, Any], candidate_document: dict[str, Any]
) -> dict[str, Any]:
    baseline = guc_parameters(baseline_document)
    candidate = guc_parameters(candidate_document)
    baseline_names = set(baseline)
    candidate_names = set(candidate)
    common = sorted(baseline_names & candidate_names)
    changed: list[dict[str, Any]] = []
    for name in common:
        fields = sorted((set(baseline[name]) | set(candidate[name])) - {"name"})
        changes = _field_changes(baseline[name], candidate[name], fields)
        if changes:
            changed.append({"name": name, "changes": changes})
    return {
        "baseline_count": len(baseline),
        "candidate_count": len(candidate),
        "unchanged_count": len(common) - len(changed),
        "added": sorted(candidate_names - baseline_names),
        "removed": sorted(baseline_names - candidate_names),
        "changed": changed,
    }


def _fingerprint(candidate: dict[str, Any]) -> str:
    facts = {field: candidate.get(field) for field in MOVE_FINGERPRINT_FIELDS}
    return json.dumps(facts, sort_keys=True, separators=(",", ":"))


def _potential_moves(
    baseline: dict[str, dict[str, Any]],
    candidate: dict[str, dict[str, Any]],
    removed: list[str],
    added: list[str],
) -> list[dict[str, str]]:
    removed_by_fingerprint: dict[str, list[str]] = {}
    added_by_fingerprint: dict[str, list[str]] = {}
    for identifier in removed:
        removed_by_fingerprint.setdefault(_fingerprint(baseline[identifier]), []).append(
            identifier
        )
    for identifier in added:
        added_by_fingerprint.setdefault(_fingerprint(candidate[identifier]), []).append(
            identifier
        )
    result = []
    for fingerprint in sorted(set(removed_by_fingerprint) & set(added_by_fingerprint)):
        old_ids = removed_by_fingerprint[fingerprint]
        new_ids = added_by_fingerprint[fingerprint]
        if len(old_ids) == 1 and len(new_ids) == 1:
            result.append(
                {
                    "baseline_id": old_ids[0],
                    "candidate_id": new_ids[0],
                    "basis": "unique semantic fingerprint",
                }
            )
    return result


def diff_backend_state(
    baseline_document: dict[str, Any], candidate_document: dict[str, Any]
) -> dict[str, Any]:
    try:
        baseline = catalog_candidates(baseline_document)
        candidate = catalog_candidates(candidate_document)
    except BackendStatePolicyError as exc:
        raise UpgradeAuditError(str(exc)) from exc
    baseline_ids = set(baseline)
    candidate_ids = set(candidate)
    common = sorted(baseline_ids & candidate_ids)
    added = sorted(candidate_ids - baseline_ids)
    removed = sorted(baseline_ids - candidate_ids)
    structural_changes: list[dict[str, Any]] = []
    identity_metadata_changes: list[dict[str, Any]] = []
    topology_changes: list[dict[str, Any]] = []
    for identifier in common:
        structural = _field_changes(
            baseline[identifier], candidate[identifier], STATE_STRUCTURAL_FIELDS
        )
        topology = _field_changes(
            baseline[identifier], candidate[identifier], STATE_TOPOLOGY_FIELDS
        )
        identity_metadata = _field_changes(
            baseline[identifier],
            candidate[identifier],
            STATE_IDENTITY_METADATA_FIELDS,
        )
        if structural:
            structural_changes.append({"id": identifier, "changes": structural})
        if identity_metadata:
            identity_metadata_changes.append(
                {"id": identifier, "changes": identity_metadata}
            )
        if topology:
            topology_changes.append({"id": identifier, "changes": topology})
    denominator = len(baseline) or 1
    return {
        "baseline_count": len(baseline),
        "candidate_count": len(candidate),
        "reused_identity_count": len(common),
        "identity_reuse_percent": round(100.0 * len(common) / denominator, 4),
        "added": added,
        "removed": removed,
        "potential_moves": _potential_moves(
            baseline, candidate, removed, added
        ),
        "structural_changes": structural_changes,
        "identity_metadata_changes": identity_metadata_changes,
        "topology_changes": topology_changes,
    }


def diff_backend_inheritance(
    baseline_document: dict[str, Any], candidate_document: dict[str, Any]
) -> dict[str, Any]:
    """Compare the source-derived EXEC_BACKEND inheritance contract."""
    expected_kind = "postgamma.backend-inheritance-catalog"
    documents = (("baseline", baseline_document), ("candidate", candidate_document))
    parsed: dict[str, dict[str, dict[str, Any]]] = {}
    contracts: dict[str, dict[str, Any]] = {}
    for label, document in documents:
        if document.get("schema_version") != 1 or document.get("kind") != expected_kind:
            raise UpgradeAuditError(
                f"{label} inheritance catalog must use schema_version 1 and kind "
                f"{expected_kind}"
            )
        states = document.get("states")
        contract = document.get("contract")
        if not isinstance(states, list) or not isinstance(contract, dict):
            raise UpgradeAuditError(
                f"{label} inheritance catalog has invalid states or contract"
            )
        by_id: dict[str, dict[str, Any]] = {}
        for index, state in enumerate(states):
            identifier = state.get("id") if isinstance(state, dict) else None
            if not isinstance(identifier, str) or not identifier or identifier in by_id:
                raise UpgradeAuditError(
                    f"{label} inheritance states[{index}] has an invalid or duplicate id"
                )
            by_id[identifier] = state
        parsed[label] = by_id
        contracts[label] = contract
    baseline = parsed["baseline"]
    candidate = parsed["candidate"]
    common = sorted(set(baseline) & set(candidate))
    changed = []
    for identifier in common:
        changes = _field_changes(
            baseline[identifier],
            candidate[identifier],
            ("name", "canonical_type"),
        )
        if changes:
            changed.append({"id": identifier, "changes": changes})
    contract_changes = _field_changes(
        contracts["baseline"],
        contracts["candidate"],
        sorted(set(contracts["baseline"]) | set(contracts["candidate"])),
    )
    added = sorted(set(candidate) - set(baseline))
    removed = sorted(set(baseline) - set(candidate))
    return {
        "baseline_count": len(baseline),
        "candidate_count": len(candidate),
        "added": added,
        "removed": removed,
        "changed": changed,
        "contract_changes": contract_changes,
        "compatible": not (added or removed or changed or contract_changes),
    }


def _managed_state_ids(
    state_document: dict[str, Any], inventory_document: dict[str, Any]
) -> set[str]:
    try:
        candidates = catalog_candidates(state_document)
        managed_usrs = guc_definition_usrs(inventory_document)
    except BackendStatePolicyError as exc:
        raise UpgradeAuditError(str(exc)) from exc
    return {
        identifier
        for identifier, candidate in candidates.items()
        if candidate["usr"] in managed_usrs
    }


def audit_policy_reuse(
    baseline_policy_document: dict[str, Any],
    baseline_state_document: dict[str, Any],
    candidate_state_document: dict[str, Any],
    baseline_inventory_document: dict[str, Any],
    candidate_inventory_document: dict[str, Any],
) -> dict[str, Any]:
    try:
        decisions = policy_decisions(baseline_policy_document)
        baseline_candidates = catalog_candidates(baseline_state_document)
        candidate_candidates = catalog_candidates(candidate_state_document)
    except BackendStatePolicyError as exc:
        raise UpgradeAuditError(str(exc)) from exc
    candidate_ids = set(candidate_candidates)
    decision_ids = set(decisions)
    baseline_managed = _managed_state_ids(
        baseline_state_document, baseline_inventory_document
    )
    candidate_managed = _managed_state_ids(
        candidate_state_document, candidate_inventory_document
    )
    reclassified_as_guc = sorted(
        identifier
        for identifier in candidate_managed
        if identifier in decisions and decisions[identifier]["owner"] != "managed_guc"
    )
    no_longer_guc = sorted(
        identifier
        for identifier in baseline_managed - candidate_managed
        if identifier in candidate_ids
    )
    return {
        "baseline_decision_count": len(decisions),
        "reusable_decision_count": len(candidate_ids & decision_ids),
        "missing_decisions": sorted(candidate_ids - decision_ids),
        "stale_required_decisions": sorted(
            identifier
            for identifier in decision_ids - candidate_ids
            if decisions[identifier]["availability"] != "conditional"
        ),
        "dormant_conditional_decisions": sorted(
            identifier
            for identifier in decision_ids - candidate_ids
            if decisions[identifier]["availability"] == "conditional"
        ),
        "reclassified_as_guc": reclassified_as_guc,
        "no_longer_guc": no_longer_guc,
        "baseline_managed_guc_count": len(baseline_managed),
        "candidate_managed_guc_count": len(candidate_managed),
    }


def _upstream_summary(document: dict[str, Any], major: int) -> dict[str, Any]:
    commit = document.get("commit")
    repository = document.get("repository")
    if not isinstance(commit, str) or not commit:
        raise UpgradeAuditError("upstream manifest has invalid commit")
    if not isinstance(repository, str) or not repository:
        raise UpgradeAuditError("upstream manifest has invalid repository")
    return {
        "repository": repository,
        "commit": commit,
        "ref_name": document.get("ref_name", ""),
        "postgresql_major": major,
    }


def integration_compatibility(engine: dict[str, Any]) -> dict[str, Any]:
    """Summarize adapter text and semantic-hook compatibility diagnostics."""
    required = (
        "baseline_generated_support_anchors",
        "candidate_generated_support_anchors",
        "baseline_runtime_hooks",
        "candidate_runtime_hooks",
        "baseline_execution_assumptions",
        "candidate_execution_assumptions",
    )
    missing = [key for key in required if key not in engine]
    if missing:
        raise UpgradeAuditError(
            "engine integration diagnostics are missing: " + ", ".join(missing)
        )
    baseline_edits = engine["baseline_generated_support_anchors"]
    candidate_edits = engine["candidate_generated_support_anchors"]
    baseline_hooks = engine["baseline_runtime_hooks"]
    candidate_hooks = engine["candidate_runtime_hooks"]
    baseline_assumptions = engine["baseline_execution_assumptions"]
    candidate_assumptions = engine["candidate_execution_assumptions"]
    collections = (
        baseline_edits,
        candidate_edits,
        baseline_hooks,
        candidate_hooks,
        baseline_assumptions,
        candidate_assumptions,
    )
    if any(not isinstance(entries, list) for entries in collections):
        raise UpgradeAuditError("engine integration diagnostics must be arrays")

    labels = (
        "baseline generated support",
        "candidate generated support",
        "baseline runtime hooks",
        "candidate runtime hooks",
        "baseline execution assumptions",
        "candidate execution assumptions",
    )
    for label, entries in zip(labels, collections):
        identifiers: set[str] = set()
        for index, entry in enumerate(entries):
            if not isinstance(entry, dict):
                raise UpgradeAuditError(f"{label}[{index}] must be an object")
            identifier = entry.get("id")
            status = entry.get("status")
            if not isinstance(identifier, str) or not identifier:
                raise UpgradeAuditError(f"{label}[{index}] has an invalid id")
            if identifier in identifiers:
                raise UpgradeAuditError(f"{label} has duplicate id {identifier}")
            identifiers.add(identifier)
            if not isinstance(status, str) or not status:
                raise UpgradeAuditError(
                    f"{label}[{index}] has an invalid status"
                )
            if status != "ok" and entry.get("severity") not in ("error", "warning"):
                raise UpgradeAuditError(
                    f"{label}[{index}] has an invalid severity"
                )
    baseline_findings = [
        entry
        for entry in baseline_edits + baseline_hooks + baseline_assumptions
        if entry["status"] != "ok"
    ]
    if baseline_findings:
        raise UpgradeAuditError(
            "baseline integration diagnostics are incompatible: "
            + ", ".join(entry["id"] for entry in baseline_findings)
        )
    findings = [
        entry
        for entry in candidate_edits + candidate_hooks + candidate_assumptions
        if isinstance(entry, dict) and entry.get("status") != "ok"
    ]
    baseline_by_id = {
        entry["id"]: entry
        for entry in baseline_edits
        if isinstance(entry, dict) and isinstance(entry.get("id"), str)
    }
    for entry in candidate_edits:
        if not isinstance(entry, dict) or entry.get("status") != "ok":
            continue
        baseline = baseline_by_id.get(entry.get("id"))
        baseline_digest = (
            baseline.get("matched_region_sha256")
            if isinstance(baseline, dict)
            else None
        )
        candidate_digest = entry.get("matched_region_sha256")
        if (
            isinstance(baseline_digest, str)
            and isinstance(candidate_digest, str)
            and baseline_digest != candidate_digest
        ):
            findings.append(
                {
                    **entry,
                    "status": "region_digest_changed",
                    "severity": "warning",
                    "baseline_region_sha256": baseline_digest,
                    "candidate_region_sha256": candidate_digest,
                }
            )
    errors = [entry for entry in findings if entry.get("severity") == "error"]
    warnings = [entry for entry in findings if entry.get("severity") == "warning"]
    return {
        "baseline": {
            "generated_support": baseline_edits,
            "runtime_hooks": baseline_hooks,
            "execution_assumptions": baseline_assumptions,
        },
        "candidate": {
            "generated_support": candidate_edits,
            "runtime_hooks": candidate_hooks,
            "execution_assumptions": candidate_assumptions,
        },
        "findings": findings,
        "summary": {
            "error_count": len(errors),
            "warning_count": len(warnings),
            "scan_ready": not errors,
            "product_ready": None,
            "validation_ready": not findings,
        },
    }


def compile_report(
    *,
    baseline_upstream: dict[str, Any],
    candidate_upstream: dict[str, Any],
    baseline_major: int,
    candidate_major: int,
    baseline_guc_catalog: dict[str, Any],
    candidate_guc_catalog: dict[str, Any],
    baseline_state_catalog: dict[str, Any],
    candidate_state_catalog: dict[str, Any],
    baseline_guc_inventory: dict[str, Any],
    candidate_guc_inventory: dict[str, Any],
    baseline_state_policy: dict[str, Any],
    baseline_inheritance_catalog: dict[str, Any],
    candidate_inheritance_catalog: dict[str, Any],
    baseline_adapter_path: Path,
    candidate_adapter_path: Path,
    upstream_diff: dict[str, Any] | None = None,
    engine: dict[str, Any] | None = None,
    inputs: dict[str, Any] | None = None,
) -> dict[str, Any]:
    try:
        baseline_adapter = load_adapter(baseline_adapter_path)
        candidate_adapter = load_adapter(candidate_adapter_path)
    except AdapterError as exc:
        raise UpgradeAuditError(str(exc)) from exc
    gucs = diff_gucs(baseline_guc_catalog, candidate_guc_catalog)
    state = diff_backend_state(baseline_state_catalog, candidate_state_catalog)
    policy = audit_policy_reuse(
        baseline_state_policy,
        baseline_state_catalog,
        candidate_state_catalog,
        baseline_guc_inventory,
        candidate_guc_inventory,
    )
    inheritance = diff_backend_inheritance(
        baseline_inheritance_catalog, candidate_inheritance_catalog
    )
    if engine is None:
        raise UpgradeAuditError("engine diagnostics are required")
    engine_document = engine
    integration = integration_compatibility(engine_document)
    review_item_count = (
        len(gucs["added"])
        + len(gucs["removed"])
        + len(gucs["changed"])
        + len(state["added"])
        + len(state["removed"])
        + len(state["structural_changes"])
        + len(policy["reclassified_as_guc"])
        + len(policy["no_longer_guc"])
        + len(integration["findings"])
        + len(inheritance["added"])
        + len(inheritance["removed"])
        + len(inheritance["changed"])
        + len(inheritance["contract_changes"])
    )
    if not inheritance["compatible"]:
        integration["summary"]["scan_ready"] = False
    return {
        "schema_version": REPORT_SCHEMA_VERSION,
        "kind": REPORT_KIND,
        "baseline": _upstream_summary(baseline_upstream, baseline_major),
        "candidate": _upstream_summary(candidate_upstream, candidate_major),
        "upstream_diff": upstream_diff or {},
        "engine": engine_document,
        "inputs": inputs or {},
        "adapter": {
            "baseline_id": baseline_adapter["id"],
            "candidate_id": candidate_adapter["id"],
            "baseline_sha256": adapter_sha256(baseline_adapter_path),
            "candidate_sha256": adapter_sha256(candidate_adapter_path),
            "changed": baseline_adapter_path.read_bytes()
            != candidate_adapter_path.read_bytes(),
        },
        "compatibility": {
            "candidate_mode": "upgrade_probe",
            "baseline_product_majors": baseline_adapter[
                "supported_postgresql_majors"
            ],
            "candidate_product_majors": candidate_adapter[
                "supported_postgresql_majors"
            ],
            "candidate_is_declared_product_major": candidate_major
            in candidate_adapter["supported_postgresql_majors"],
        },
        "integration_compatibility": integration,
        "gucs": gucs,
        "backend_state": state,
        "backend_inheritance": inheritance,
        "policy_reuse": policy,
        "summary": {
            "review_item_count": review_item_count,
            "generic_engine_change_required": False,
            "candidate_policy_is_reviewed": False,
        },
    }


def write_report(report: dict[str, Any], output: Path) -> None:
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(
        json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )


def print_summary(report: dict[str, Any], output: Path) -> None:
    baseline = report["baseline"]
    candidate = report["candidate"]
    gucs = report["gucs"]
    state = report["backend_state"]
    policy = report["policy_reuse"]
    integration = report["integration_compatibility"]["summary"]

    def readiness(value: object) -> str:
        if value is None:
            return "unknown"
        return str(bool(value)).lower()
    print(
        f"upgrade audit: PostgreSQL {baseline['postgresql_major']} "
        f"{baseline['commit'][:12]} -> PostgreSQL {candidate['postgresql_major']} "
        f"{candidate['commit'][:12]}"
    )
    compatibility = report["compatibility"]
    print(
        "  candidate mode: upgrade probe; product majors="
        + ",".join(
            str(value) for value in compatibility["candidate_product_majors"]
        )
    )
    print(
        f"  GUC: {gucs['baseline_count']} -> {gucs['candidate_count']}; "
        f"added={len(gucs['added'])}, removed={len(gucs['removed'])}, "
        f"changed={len(gucs['changed'])}"
    )
    print(
        f"  state: {state['baseline_count']} -> {state['candidate_count']}; "
        f"identity reuse={state['identity_reuse_percent']:.2f}%, "
        f"added={len(state['added'])}, removed={len(state['removed'])}, "
        f"structural={len(state['structural_changes'])}, "
        f"USR-only={len(state['identity_metadata_changes'])}"
    )
    print(
        f"  policy review: missing={len(policy['missing_decisions'])}, "
        f"stale={len(policy['stale_required_decisions'])}, "
        f"GUC reclassifications={len(policy['reclassified_as_guc'])}"
    )
    print(
        f"  integration: errors={integration['error_count']}, "
        f"warnings={integration['warning_count']}, "
        f"scan_ready={readiness(integration['scan_ready'])}, "
        f"product_ready={readiness(integration['product_ready'])}, "
        f"validation_ready={readiness(integration['validation_ready'])}"
    )
    print(f"  report: {output}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--baseline-upstream", required=True, type=Path)
    parser.add_argument("--candidate-upstream", required=True, type=Path)
    parser.add_argument("--baseline-major", required=True, type=int)
    parser.add_argument("--candidate-major", required=True, type=int)
    parser.add_argument("--baseline-guc-catalog", required=True, type=Path)
    parser.add_argument("--candidate-guc-catalog", required=True, type=Path)
    parser.add_argument("--baseline-state-catalog", required=True, type=Path)
    parser.add_argument("--candidate-state-catalog", required=True, type=Path)
    parser.add_argument("--baseline-guc-inventory", required=True, type=Path)
    parser.add_argument("--candidate-guc-inventory", required=True, type=Path)
    parser.add_argument("--baseline-state-policy", required=True, type=Path)
    parser.add_argument("--baseline-inheritance-catalog", required=True, type=Path)
    parser.add_argument("--candidate-inheritance-catalog", required=True, type=Path)
    parser.add_argument("--baseline-adapter", required=True, type=Path)
    parser.add_argument("--candidate-adapter", required=True, type=Path)
    parser.add_argument("--engine", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    try:
        report = compile_report(
            baseline_upstream=load_json(args.baseline_upstream),
            candidate_upstream=load_json(args.candidate_upstream),
            baseline_major=args.baseline_major,
            candidate_major=args.candidate_major,
            baseline_guc_catalog=load_json(args.baseline_guc_catalog),
            candidate_guc_catalog=load_json(args.candidate_guc_catalog),
            baseline_state_catalog=load_json(args.baseline_state_catalog),
            candidate_state_catalog=load_json(args.candidate_state_catalog),
            baseline_guc_inventory=load_json(args.baseline_guc_inventory),
            candidate_guc_inventory=load_json(args.candidate_guc_inventory),
            baseline_state_policy=load_json(args.baseline_state_policy),
            baseline_inheritance_catalog=load_json(
                args.baseline_inheritance_catalog
            ),
            candidate_inheritance_catalog=load_json(
                args.candidate_inheritance_catalog
            ),
            baseline_adapter_path=args.baseline_adapter.resolve(),
            candidate_adapter_path=args.candidate_adapter.resolve(),
            engine=load_json(args.engine),
        )
    except UpgradeAuditError as exc:
        parser.error(str(exc))
    output = args.output.resolve()
    write_report(report, output)
    print_summary(report, output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
