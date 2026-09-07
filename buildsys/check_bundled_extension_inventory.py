#!/usr/bin/env python3
"""Close the manifest, symbol, resource, and state inventory for embedded release modules."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
from typing import Any

from bundle_embedded_static_modules import (
    RECEIPT_KIND,
    read_json,
    validate_manifest,
)
from check_embedded_lifecycle import input_identity, write_json
from generate_extension_state_plan import (
    POLICY_KIND as VIRTUALIZED_POLICY_KIND,
    REPORT_KIND as VIRTUALIZED_REPORT_KIND,
    parse_policy as parse_virtualized_policy,
)


EVIDENCE_KIND = "postgamma.bundled-extension-inventory"
CATALOG_KIND = "postgamma.backend-mutable-state-catalog"
POLICY_KIND = "postgamma.extension-state-policy"


class BundledExtensionInventoryError(RuntimeError):
    """The declared bundled-extension closure and derived facts disagree."""


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def beneath(root: Path, value: str, label: str) -> Path:
    path = (root / value).resolve()
    try:
        path.relative_to(root)
    except ValueError as exc:
        raise BundledExtensionInventoryError(
            f"{label} escapes the project root"
        ) from exc
    if not path.is_file():
        raise BundledExtensionInventoryError(f"{label} is missing: {value}")
    return path


def validate_policy(
    path: Path, extension_id: str, candidate_ids: set[str]
) -> dict[str, Any]:
    policy = read_json(path)
    if (
        policy.get("schema_version") != 1
        or policy.get("kind") != POLICY_KIND
        or policy.get("extension_id") != extension_id
        or set(policy) != {
            "schema_version",
            "kind",
            "extension_id",
            "contract",
        }
    ):
        raise BundledExtensionInventoryError(
            f"{extension_id}: state policy identity or schema is invalid"
        )
    contract = policy.get("contract")
    if not isinstance(contract, dict):
        raise BundledExtensionInventoryError(
            f"{extension_id}: state policy is incomplete"
        )
    if candidate_ids:
        raise BundledExtensionInventoryError(
            f"{extension_id}: mutable static storage must be moved behind the "
            "instance or session SDK before bundling: "
            + ", ".join(sorted(candidate_ids))
        )
    expected_contract = {
        "file_scope_mutable_state": 0,
        "instance_state": "pgmex instance shared memory",
        "session_state": "pgmex logical-session state",
        "unknown_state": "fail-closed",
    }
    if contract != expected_contract:
        raise BundledExtensionInventoryError(
            f"{extension_id}: state ownership contract changed"
        )
    return {
        "extension_id": extension_id,
        "policy": path.as_posix(),
        "policy_sha256": sha256(path),
        "mutable_static_candidates": 0,
    }


def validate_virtualized_policy(
    path: Path, extension_id: str, report_path: Path
) -> dict[str, Any]:
    policy_document = read_json(path)
    try:
        policy = parse_virtualized_policy(policy_document)
    except ValueError as exc:
        raise BundledExtensionInventoryError(str(exc)) from exc
    if policy["extension_id"] != extension_id:
        raise BundledExtensionInventoryError(
            f"{extension_id}: virtualized state policy has the wrong extension id"
        )
    report = read_json(report_path)
    candidates = report.get("candidates")
    observed_ids = {
        candidate.get("id")
        for candidate in candidates or []
        if isinstance(candidate, dict) and isinstance(candidate.get("id"), str)
    }
    summary = report.get("summary")
    report_selection = report.get("selection")
    kernel_references = report.get("kernel_references")
    if (
        report.get("schema_version") != 1
        or report.get("kind") != VIRTUALIZED_REPORT_KIND
        or report.get("status") != "pass"
        or report.get("extension_id") != extension_id
        or report.get("upstream") != policy["upstream"]
        or not isinstance(candidates, list)
        or len(observed_ids) != len(candidates)
        or observed_ids != set(policy["decisions"])
        or not isinstance(summary, dict)
        or summary.get("candidate_count") != len(candidates)
        or summary.get("role_candidate_count") <= 0
        or summary.get("replacement_count") <= 0
        or summary.get("static_initializer_use_count") != 0
        or not isinstance(summary.get("kernel_reference_count"), int)
        or summary["kernel_reference_count"] < 0
        or not isinstance(summary.get("kernel_reference_symbol_count"), int)
        or summary["kernel_reference_symbol_count"] < 0
        or any(
            not isinstance(summary.get(name), int) or summary[name] < 0
            for name in (
                "kernel_state_replacement_count",
                "kernel_guc_replacement_count",
                "kernel_immutable_reference_count",
            )
        )
        or summary["kernel_reference_count"]
        != summary["kernel_state_replacement_count"]
        + summary["kernel_guc_replacement_count"]
        + summary["kernel_immutable_reference_count"]
        or not isinstance(kernel_references, list)
        or any(
            not isinstance(identifier, str)
            or not identifier.startswith("external:")
            for identifier in kernel_references
        )
        or len(kernel_references) != len(set(kernel_references))
        or len(kernel_references) != summary["kernel_reference_symbol_count"]
        or not isinstance(report_selection, dict)
        or report_selection.get("domain") != extension_id
        or not isinstance(report_selection.get("translation_units"), int)
        or report_selection["translation_units"] <= 0
    ):
        raise BundledExtensionInventoryError(
            f"{extension_id}: virtualized state evidence is incomplete or stale"
        )
    return {
        "extension_id": extension_id,
        "policy": path.as_posix(),
        "policy_sha256": sha256(path),
        "state_report": report_path.as_posix(),
        "state_report_sha256": sha256(report_path),
        "mutable_static_candidates": len(candidates),
        "virtualized_role_candidates": summary["role_candidate_count"],
        "replacement_count": summary["replacement_count"],
        "kernel_reference_count": summary["kernel_reference_count"],
        "kernel_reference_symbol_count": summary[
            "kernel_reference_symbol_count"
        ],
        "translation_units": report_selection["translation_units"],
    }


def validate_bundle(
    manifest_path: Path,
    manifest: dict[str, Any],
    bundle_path: Path,
) -> dict[str, Any]:
    bundle = read_json(bundle_path)
    if (
        bundle.get("schema_version") != 1
        or bundle.get("kind") != RECEIPT_KIND
        or bundle.get("manifest_id") != manifest["id"]
        or bundle.get("manifest_sha256") != sha256(manifest_path)
        or bundle.get("postgresql_major") != manifest["postgresql_major"]
        or bundle.get("capability_vocabulary")
        != manifest["capability_vocabulary"]
    ):
        raise BundledExtensionInventoryError(
            "static-module bundle does not identify the current manifest"
        )
    reports = bundle.get("modules")
    if not isinstance(reports, list):
        raise BundledExtensionInventoryError("static-module reports are missing")
    by_id = {
        report.get("id"): report
        for report in reports
        if isinstance(report, dict) and isinstance(report.get("id"), str)
    }
    if len(by_id) != len(reports) or set(by_id) != {
        module["id"] for module in manifest["modules"]
    }:
        raise BundledExtensionInventoryError(
            "static-module bundle membership differs from the manifest"
        )
    exact_fields = (
        "logical_name",
        "version",
        "postgresql_major",
        "sdk_contract",
        "sdk_abi_version",
        "capabilities",
        "capability_bits",
        "lifecycle",
        "lifecycle_bits",
        "build",
        "state_policy",
        "resources",
        "dependencies",
    )
    for module in manifest["modules"]:
        report = by_id[module["id"]]
        for field in exact_fields:
            if report.get(field) != module[field]:
                raise BundledExtensionInventoryError(
                    f"{module['id']}: bundle field {field} differs from the manifest"
                )
        expected_symbols = sorted(
            symbol["linker_name"] for symbol in module["symbols"]
        )
        if report.get("registered_symbols") != expected_symbols:
            raise BundledExtensionInventoryError(
                f"{module['id']}: registered symbol closure changed"
            )
    for path_field, hash_field in (
        ("output", "output_sha256"),
        ("facts", "facts_sha256"),
    ):
        raw_path = bundle.get(path_field)
        if not isinstance(raw_path, str):
            raise BundledExtensionInventoryError(
                f"static-module bundle has no {path_field} path"
            )
        artifact = Path(raw_path).resolve(strict=True)
        if sha256(artifact) != bundle.get(hash_field):
            raise BundledExtensionInventoryError(
                f"static-module bundle {path_field} identity changed"
            )
    return {
        "manifest_id": manifest["id"],
        "module_count": len(reports),
        "sdk_module_count": sum(
            bool(module["sdk_contract"]) for module in manifest["modules"]
        ),
        "registered_symbol_count": sum(
            len(report["registered_symbols"]) for report in reports
        ),
        "manifest_sha256": sha256(manifest_path),
        "bundle_sha256": sha256(bundle_path),
    }


def validate_inventory(
    project_root: Path,
    manifest_path: Path,
    bundle_path: Path,
    catalog_path: Path,
    selection_path: Path,
    state_reports: dict[str, Path] | None = None,
) -> dict[str, Any]:
    manifest = validate_manifest(read_json(manifest_path))
    if manifest["schema_version"] != 2 or manifest["postgresql_major"] != 19:
        raise BundledExtensionInventoryError(
            "embedded release requires the PostgreSQL 19 static-module manifest v2"
        )
    bundle = validate_bundle(manifest_path, manifest, bundle_path)
    catalog = read_json(catalog_path)
    if catalog.get("schema_version") != 1 or catalog.get("kind") != CATALOG_KIND:
        raise BundledExtensionInventoryError("extension state catalog is invalid")
    candidates = catalog.get("candidates")
    if not isinstance(candidates, list):
        raise BundledExtensionInventoryError(
            "extension state catalog has no candidate array"
        )
    candidate_ids = {
        item.get("id")
        for item in candidates
        if isinstance(item, dict) and isinstance(item.get("id"), str)
    }
    if len(candidate_ids) != len(candidates):
        raise BundledExtensionInventoryError(
            "extension state catalog has invalid or duplicate candidates"
        )
    selection = read_json(selection_path)
    selected_files = selection.get("files")
    sdk_modules = [
        module for module in manifest["modules"] if module["sdk_contract"]
    ]
    legacy_modules: list[dict[str, Any]] = []
    virtualized_modules: list[tuple[dict[str, Any], Path]] = []
    for module in sdk_modules:
        raw_policy = module["state_policy"]
        if not raw_policy:
            raise BundledExtensionInventoryError(
                f"{module['id']}: SDK module has no state policy"
            )
        policy_path = beneath(
            project_root, raw_policy, f"{module['id']} state policy"
        )
        policy_document = read_json(policy_path)
        if policy_document.get("kind") == POLICY_KIND:
            legacy_modules.append(module)
        elif policy_document.get("kind") == VIRTUALIZED_POLICY_KIND:
            virtualized_modules.append((module, policy_path))
        else:
            raise BundledExtensionInventoryError(
                f"{module['id']}: unknown extension state policy kind"
            )
    expected_sources = sorted(
        module["build"]["source"] for module in legacy_modules
    )
    observed_sources = sorted(
        item.get("path")
        for item in selected_files or []
        if isinstance(item, dict) and isinstance(item.get("path"), str)
    )
    if (
        selection.get("domain") != "postgamma-release-extension"
        or selection.get("strategy") != "source-domain"
        or selection.get("selected_translation_units") != len(expected_sources)
        or observed_sources != expected_sources
    ):
        raise BundledExtensionInventoryError(
            "extension AST selection is not the exact SDK source domain"
        )
    policies: list[dict[str, Any]] = []
    for module in legacy_modules:
        policy_path = beneath(
            project_root,
            module["state_policy"],
            f"{module['id']} state policy",
        )
        policies.append(validate_policy(policy_path, module["id"], candidate_ids))
    reports = state_reports or {}
    expected_report_ids = {module["id"] for module, _path in virtualized_modules}
    if set(reports) != expected_report_ids:
        missing = sorted(expected_report_ids - set(reports))
        stale = sorted(set(reports) - expected_report_ids)
        raise BundledExtensionInventoryError(
            "virtualized state report set changed: "
            f"missing={missing}, stale={stale}"
        )
    for module, policy_path in virtualized_modules:
        policies.append(
            validate_virtualized_policy(
                policy_path, module["id"], reports[module["id"]]
            )
        )
    virtualized_candidates = sum(
        item.get("virtualized_role_candidates", 0) for item in policies
    )
    return {
        "schema_version": 1,
        "kind": EVIDENCE_KIND,
        "status": "pass",
        "postgresql_major": 19,
        "bundle": bundle,
        "ast": {
            "translation_units": len(expected_sources)
            + sum(item.get("translation_units", 0) for item in policies),
            "mutable_static_candidates": len(candidate_ids)
            + virtualized_candidates,
            "virtualized_role_candidates": virtualized_candidates,
            "unknown_candidates": 0,
            "selection_sha256": sha256(selection_path),
            "catalog_sha256": sha256(catalog_path),
        },
        "policies": policies,
        "inputs": input_identity(
            [
                manifest_path,
                bundle_path,
                catalog_path,
                selection_path,
                *(Path(item["policy"]) for item in policies),
                *(Path(item["state_report"]) for item in policies if "state_report" in item),
            ]
        ),
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--project-root", required=True, type=Path)
    parser.add_argument("--manifest", required=True, type=Path)
    parser.add_argument("--bundle", required=True, type=Path)
    parser.add_argument("--catalog", required=True, type=Path)
    parser.add_argument("--selection", required=True, type=Path)
    parser.add_argument(
        "--state-report",
        action="append",
        default=[],
        metavar="EXTENSION_ID=PATH",
    )
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    try:
        state_reports: dict[str, Path] = {}
        for mapping in args.state_report:
            if "=" not in mapping:
                raise BundledExtensionInventoryError(
                    f"invalid --state-report mapping: {mapping}"
                )
            extension_id, raw_path = mapping.split("=", 1)
            if not extension_id or extension_id in state_reports or not raw_path:
                raise BundledExtensionInventoryError(
                    f"invalid --state-report mapping: {mapping}"
                )
            state_reports[extension_id] = Path(raw_path).resolve(strict=True)
        document = validate_inventory(
            args.project_root.resolve(strict=True),
            args.manifest.resolve(strict=True),
            args.bundle.resolve(strict=True),
            args.catalog.resolve(strict=True),
            args.selection.resolve(strict=True),
            state_reports,
        )
        write_json(args.output.resolve(), document)
    except (BundledExtensionInventoryError, OSError, ValueError) as exc:
        parser.error(str(exc))
    print(
        "bundled extension inventory: pass "
        "(manifest-exact symbols/resources, reviewed mutable state)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
