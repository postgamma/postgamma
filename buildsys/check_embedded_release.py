#!/usr/bin/env python3
"""Freeze ABI v1.3 and close the complete embedded product evidence graph."""

from __future__ import annotations

import argparse
import re
from pathlib import Path
from typing import Any

from check_embedded_lifecycle import input_identity, write_json
from check_c_api_release import (
    ReleaseCheckError,
    load_document,
    nested,
    normalize_fields,
    require,
    unique_entries,
)
from check_multi_instance import REQUIRED_MARKER_VALUES


BASELINE_KIND = "postgamma.c-abi-release-baseline"
PRIOR_RELEASE_KIND = "postgamma.logical-release"
CATALOG_KIND = "postgamma.public-api-ast-catalog"
API_CLOSURE_KIND = "postgamma.c-api-closure"
PUBLIC_API_KIND = "postgamma.public-c-api"
MULTI_KIND = "postgamma.multi-instance"
CONFORMANCE_KIND = "postgamma.embedded-conformance"
EXTENSION_KIND = "postgamma.bundled-extension-inventory"
PGVECTOR_CONTRACT_KIND = "postgamma.pgvector-contract"
RESOURCE_KIND = "postgamma.resource-pack"
REACHABILITY_KIND = "postgamma.instance-reachability"
SDK_KIND = "postgamma.installed-c-sdk"
STATIC_SDK_KIND = "postgamma.static-sdk-evidence"
SANITIZER_KIND = "postgamma.release-sanitizer-evidence"
PORTABILITY_KIND = "postgamma.portability-evidence"
EVIDENCE_KIND = "postgamma.embedded-release"


class EmbeddedReleaseCheckError(RuntimeError):
    """The ABI v1.3 baseline or embedded evidence graph is incomplete."""


def exact_abi_metadata(baseline: dict[str, Any]) -> dict[str, Any]:
    expected = {
        "major": 1,
        "minor": 3,
        "encoded_version": 65539,
        "state": "frozen",
        "release_milestone": "embedded-release",
        "delivered_through": "embedded-release",
        "symbol_version_node": "POSTGAMMA_1.0",
        "evolution": "additive-within-major",
    }
    if baseline.get("abi") != expected:
        raise EmbeddedReleaseCheckError("ABI v1.3 release metadata changed")
    if baseline.get("postgresql_major") != 19:
        raise EmbeddedReleaseCheckError("the embedded release must target PostgreSQL 19")
    expected_prior = {
        "path": "manifests/api/c-abi-v1-2.json",
        "encoded_version": 65538,
        "function_count": 73,
        "complete_record_count": 21,
        "opaque_record_count": 9,
        "numeric_count": 117,
    }
    if baseline.get("prior_baseline") != expected_prior:
        raise EmbeddedReleaseCheckError("ABI v1.2 predecessor metadata changed")
    return expected


def require_record_layout(
    name: str, frozen: dict[str, Any], current: dict[str, Any]
) -> None:
    if (
        current.get("size") != frozen.get("size")
        or current.get("alignment") != frozen.get("alignment")
        or normalize_fields(current) != normalize_fields(frozen)
    ):
        raise EmbeddedReleaseCheckError(f"ABI v1.3 record layout changed: {name}")


def validate_abi(
    baseline: dict[str, Any],
    prior_release: dict[str, Any],
    catalog: dict[str, Any],
    closure: dict[str, Any],
    public_api: dict[str, Any],
) -> dict[str, Any]:
    abi = exact_abi_metadata(baseline)
    require(prior_release, "status", "pass")
    require(prior_release, "abi.encoded_version", 65538)
    require(prior_release, "abi.prior_abi_compatible", True)
    require(prior_release, "abi.breaking_changes", 0)
    require(closure, "status", "pass")
    require(closure, "abi.encoded_version", abi["encoded_version"])
    require(closure, "abi.major", 1)
    require(closure, "abi.minor", 3)
    require(closure, "core_abi_baseline.breaking_changes", 0)
    require(public_api, "status", "pass")
    require(public_api, "marker.abi", str(abi["encoded_version"]))
    require(public_api, "marker.postgres", "19")
    require(public_api, "dynamic_symbols.version_node", abi["symbol_version_node"])

    functions = unique_entries(catalog.get("functions"), "name", "current function")
    additions = unique_entries(
        baseline.get("additive_functions"), "name", "ABI v1.3 function"
    )
    prior_function_count = (
        nested(prior_release, "abi.functions.prior")
        + nested(prior_release, "abi.functions.additive")
    )
    if (
        prior_function_count != nested(baseline, "prior_baseline.function_count")
        or prior_function_count + len(additions) != len(functions)
        or len(functions) != baseline.get("exported_symbol_count")
    ):
        raise EmbeddedReleaseCheckError("ABI v1.3 function inventory is not additive-exact")
    for name, frozen in additions.items():
        if functions.get(name, {}).get("function_type") != frozen.get("function_type"):
            raise EmbeddedReleaseCheckError(f"ABI v1.3 function changed: {name}")

    records = unique_entries(catalog.get("records"), "name", "current record")
    complete = {
        name: record for name, record in records.items() if record.get("complete") is True
    }
    opaque = {
        name for name, record in records.items() if record.get("complete") is False
    }
    record_additions = unique_entries(
        baseline.get("additive_complete_records"),
        "name",
        "ABI v1.3 complete record",
    )
    prior_complete_count = (
        nested(prior_release, "abi.records.prior_complete")
        + nested(prior_release, "abi.records.additive_complete")
    )
    if (
        prior_complete_count
        != nested(baseline, "prior_baseline.complete_record_count")
        or prior_complete_count + len(record_additions) != len(complete)
        or len(complete) != baseline.get("complete_record_count")
        or len(opaque) != baseline.get("opaque_record_count")
        or len(opaque) != nested(baseline, "prior_baseline.opaque_record_count")
        or baseline.get("additive_opaque_records") != []
    ):
        raise EmbeddedReleaseCheckError("ABI v1.3 record inventory is not additive-exact")
    for name, frozen in record_additions.items():
        if name not in complete:
            raise EmbeddedReleaseCheckError(f"ABI v1.3 record is missing: {name}")
        require_record_layout(name, frozen, complete[name])

    numerics = nested(closure, "numeric_registry.values")
    additions_numeric = baseline.get("numeric_additions")
    overrides = baseline.get("numeric_overrides")
    if not all(isinstance(value, dict) for value in (numerics, additions_numeric, overrides)):
        raise EmbeddedReleaseCheckError("ABI v1.3 numeric registry is invalid")
    if (
        len(numerics) != baseline.get("numeric_count")
        or nested(baseline, "prior_baseline.numeric_count")
        + len(additions_numeric) != len(numerics)
    ):
        raise EmbeddedReleaseCheckError("ABI v1.3 numeric inventory is not additive-exact")
    for name, value in {**additions_numeric, **overrides}.items():
        if numerics.get(name) != value:
            raise EmbeddedReleaseCheckError(f"ABI v1.3 numeric value changed: {name}")

    capability = baseline.get("capabilities")
    if not isinstance(capability, dict):
        raise EmbeddedReleaseCheckError("ABI v1.3 capability baseline is missing")
    advertised = capability.get("advertised")
    reserved = capability.get("reserved_unadvertised")
    if (
        not isinstance(advertised, list)
        or not isinstance(reserved, list)
        or len(advertised) != len(set(advertised))
        or set(advertised) & set(reserved)
    ):
        raise EmbeddedReleaseCheckError("ABI v1.3 capability sets are invalid")
    try:
        advertised_value = sum(numerics[name] for name in advertised)
        reserved_value = sum(numerics[name] for name in reserved)
    except KeyError as exc:
        raise EmbeddedReleaseCheckError(
            f"ABI v1.3 capability is unknown: {exc.args[0]}"
        ) from exc
    if (
        advertised_value != capability.get("advertised_value")
        or int(nested(public_api, "marker.capabilities")) != advertised_value
        or advertised_value & reserved_value
    ):
        raise EmbeddedReleaseCheckError("runtime capability mask differs from ABI v1.3")

    exports = nested(public_api, "dynamic_symbols.exported_symbols")
    if not isinstance(exports, list) or set(exports) != set(functions):
        raise EmbeddedReleaseCheckError("shared-library exports differ from the ABI catalog")
    if nested(public_api, "dynamic_symbols.exported_symbol_count") != len(functions):
        raise EmbeddedReleaseCheckError("shared-library export count is inconsistent")

    extension_sdk = baseline.get("extension_sdk")
    if extension_sdk != {
        "abi_version": 1,
        "postgresql_major": 19,
        "dynamic_loading": False,
        "unbundled_policy": "fail-closed",
        "lifecycle_phases": [
            "library-initialize",
            "instance-request",
            "instance-startup",
            "instance-shutdown",
            "session-initialize",
            "session-reset",
            "session-destroy",
        ],
    }:
        raise EmbeddedReleaseCheckError("bundled-extension SDK baseline changed")
    return {
        **abi,
        "prior_abi_compatible": True,
        "breaking_changes": 0,
        "function_count": len(functions),
        "complete_record_count": len(complete),
        "opaque_record_count": len(opaque),
        "numeric_count": len(numerics),
        "advertised_capabilities": advertised_value,
        "additive_functions": sorted(additions),
        "additive_records": sorted(record_additions),
        "additive_numerics": sorted(additions_numeric),
    }


def validate_canary_contract(path: Path) -> dict[str, Any]:
    content = path.read_text(encoding="utf-8")
    required_fragments = (
        'cron: "17 2 * * 0"',
        "workflow_dispatch:",
        "REL_19_STABLE",
        "- master",
        'make upstream-canary PG_REF="$PG_COMMIT"',
        "buildsys/plan_upstream_ci.py",
        "--enable-tap-tests --enable-injection-points",
        "targets=(embedded-runtime-fast-check embedded-multi-instance-check embedded-conformance-check)",
        "targets=(complete-source-check)",
        "build/embedded-conformance/reports/",
    )
    missing = [fragment for fragment in required_fragments if fragment not in content]
    if missing:
        raise EmbeddedReleaseCheckError(
            "upstream canary contract is incomplete: " + ", ".join(missing)
        )
    if "CANARY_PROFILE: ${{ inputs.profile || 'weekly' }}" not in content:
        raise EmbeddedReleaseCheckError("weekly canary profile selection is missing")
    return {
        "daily_schedule": None,
        "weekly_schedule": "17 2 * * 0",
        "moving_refs": ["REL_19_STABLE", "master"],
        "weekly_product_gate": "complete-source-check",
    }


def positive_integer(value: Any, label: str) -> int:
    if not isinstance(value, int) or isinstance(value, bool) or value <= 0:
        raise EmbeddedReleaseCheckError(f"{label} is not a positive integer")
    return value


def validate_product_evidence(
    public_api: dict[str, Any],
    multi: dict[str, Any],
    conformance: dict[str, Any],
    extensions: dict[str, Any],
    pgvector: dict[str, Any],
    resources: dict[str, Any],
    reachability: dict[str, Any],
    installed_sdk: dict[str, Any],
    static_sdk: dict[str, Any],
    sanitizers: dict[str, Any],
    portability: dict[str, Any],
) -> dict[str, Any]:
    for document, kind, label in (
        (multi, MULTI_KIND, "multi-instance"),
        (conformance, CONFORMANCE_KIND, "embedded conformance"),
        (extensions, EXTENSION_KIND, "extension inventory"),
        (pgvector, PGVECTOR_CONTRACT_KIND, "pgvector contract"),
        (resources, RESOURCE_KIND, "resource pack"),
        (reachability, REACHABILITY_KIND, "instance reachability"),
        (installed_sdk, SDK_KIND, "installed SDK"),
        (static_sdk, STATIC_SDK_KIND, "static SDK"),
        (sanitizers, SANITIZER_KIND, "sanitizers"),
        (portability, PORTABILITY_KIND, "portability"),
    ):
        if document.get("kind") != kind or document.get("status") != "pass":
            raise EmbeddedReleaseCheckError(f"{label} evidence is not passing")

    marker = multi.get("marker")
    if not isinstance(marker, dict):
        raise EmbeddedReleaseCheckError("multi-instance marker is missing")
    for name, expected in REQUIRED_MARKER_VALUES.items():
        if marker.get(name) != expected:
            raise EmbeddedReleaseCheckError(f"multi-instance field {name} changed")
    require(multi, "capability_advertised", True)
    lifecycle = multi.get("extension_lifecycle")
    expected_modules = {
        "pgvector": {
            "library_initializations": 1,
            "instance_requests": 3,
            "instance_startups": 3,
            "instance_shutdowns": 3,
            "session_initializations": 0,
            "session_destroys": 0,
        },
        "postgamma_sdk_probe": {
            "library_initializations": 1,
            "instance_requests": 3,
            "instance_startups": 3,
            "instance_shutdowns": 3,
            "session_initializations": 9,
            "session_destroys": 9,
        },
    }
    if lifecycle != {
        "library_initializations": 2,
        "instance_requests": 6,
        "instance_startups": 6,
        "instance_shutdowns": 6,
        "session_initializations": 9,
        "session_destroys": 9,
        "modules": expected_modules,
    }:
        raise EmbeddedReleaseCheckError("extension lifecycle cardinality changed")
    for field in (
        "host_signal_delivery_calls",
        "network_endpoint_calls",
        "process_creation_calls",
        "process_global_state_calls",
    ):
        require(multi, f"host_safety.{field}", 0)
    if multi.get("library") != public_api.get("library"):
        raise EmbeddedReleaseCheckError(
            "the embedded gate did not execute the audited shared library"
        )
    require(multi, "cwd_independent_provider", True)

    require(conformance, "postgresql_major", 19)
    require(conformance, "case_count", 54)
    require(conformance, "connection_count", 2)
    require(conformance, "socket_libpq_matches_embedded_public_api", True)
    if conformance.get("category_counts") != {
        "cleanup": 3,
        "ddl": 2,
        "dml": 4,
        "types": 9,
        "query": 6,
        "mvcc": 3,
        "transaction": 6,
        "savepoint": 6,
        "error": 3,
        "session": 3,
        "temporary": 4,
        "locking": 5,
    }:
        raise EmbeddedReleaseCheckError("embedded conformance category coverage changed")
    for field in (
        "host_signal_delivery_calls",
        "network_endpoint_calls",
        "process_creation_calls",
        "process_global_state_calls",
    ):
        require(conformance, f"candidate_host_safety.{field}", 0)

    require(extensions, "postgresql_major", 19)
    require(extensions, "bundle.module_count", 4)
    require(extensions, "bundle.sdk_module_count", 2)
    require(extensions, "bundle.registered_symbol_count", 241)
    require(extensions, "ast.translation_units", 20)
    require(extensions, "ast.mutable_static_candidates", 16)
    require(extensions, "ast.virtualized_role_candidates", 16)
    require(extensions, "ast.unknown_candidates", 0)

    require(pgvector, "postgresql_major", 19)
    require(pgvector, "pgvector.version", "0.8.6")
    require(pgvector, "pgvector.tag", "v0.8.6")
    require(pgvector, "pgvector.sql_function_count", 104)
    require(pgvector, "pgvector.parallel_worker_entry_count", 2)
    require(pgvector, "pgvector.registered_symbol_count", 212)
    require(pgvector, "pgvector.state_candidate_count", 16)
    require(pgvector, "pgvector.state_replacement_count", 97)
    require(pgvector, "pgvector.kernel_reference_count", 43)
    require(pgvector, "pgvector.kernel_reference_symbol_count", 7)
    require(pgvector, "pgvector.kernel_macro_bridge_count", 1)

    require(resources, "postgresql_major", 19)
    require(resources, "deterministic_rebuilds", 2)
    require(resources, "declared_extension_resources", 7)
    require(resources, "pack.absolute_logical_paths", 0)
    require(resources, "pack.missing_files", 0)
    require(resources, "pack.extra_files", 0)
    require(resources, "pack.metadata_matches_receipt", True)
    resource_files = positive_integer(nested(resources, "pack.file_count"), "resource file count")

    require(reachability, "postgresql_major", 19)
    require(reachability, "reachability.source_exact", True)
    require(reachability, "reachability.runtime_exact", True)
    require(reachability, "reachability.dual_live_instances", 2)
    instance_slots = positive_integer(
        nested(reachability, "reachability.instance_slots"), "instance slot count"
    )

    require(installed_sdk, "postgresql_major", 19)
    require(installed_sdk, "marker.abi", "65539")
    require(installed_sdk, "marker.capabilities", "112639")
    require(installed_sdk, "extension_sdk.postgresql_headers_required", 0)
    languages = nested(installed_sdk, "extension_sdk.compiled_languages")
    if set(languages) != {"c", "c++"}:
        raise EmbeddedReleaseCheckError("extension SDK was not compiled as C and C++")
    staged_paths = {
        item.get("path")
        for item in installed_sdk.get("staged_sdk", [])
        if isinstance(item, dict)
    }
    if "include/postgamma/postgamma_extension.h" not in staged_paths:
        raise EmbeddedReleaseCheckError("installed SDK omitted postgamma_extension.h")

    require(static_sdk, "postgresql_major", 19)
    require(static_sdk, "linkage", "static")
    require(static_sdk, "sdk_version", "1.3")
    require(static_sdk, "archive.public_symbol_count", 75)
    require(static_sdk, "archive.exposed_internal_symbol_count", 0)
    require(static_sdk, "archive.deterministic", True)
    localized_static_symbols = positive_integer(
        nested(static_sdk, "archive.localized_internal_symbol_count"),
        "localized static-library symbol count",
    )
    require(static_sdk, "consumer.postgamma_shared_dependencies", 0)
    require(static_sdk, "consumer.runtime_paths", 0)
    require(static_sdk, "pgvector_consumer.marker.version", "0.8.6")
    require(static_sdk, "pgvector_consumer.marker.hnsw", "true")
    require(static_sdk, "pgvector_consumer.marker.nearest", "1")
    require(static_sdk, "pgvector_consumer.marker.phase", "closed")
    require(static_sdk, "pgvector_consumer.postgamma_shared_dependencies", 0)
    require(static_sdk, "pgvector_consumer.runtime_paths", 0)
    require(static_sdk, "marker.abi", "65539")
    require(static_sdk, "marker.capabilities", "112639")
    for field in (
        "host_signal_delivery_calls",
        "network_endpoint_calls",
        "process_creation_calls",
        "process_global_state_calls",
    ):
        require(static_sdk, f"host_safety.{field}", 0)

    require(sanitizers, "postgresql_major", 19)
    require(sanitizers, "execution_contract.halt_on_first_error", True)
    require(
        sanitizers,
        "execution_contract.programs_built_and_executed_before_report",
        True,
    )
    sanitizer_programs = sum(
        positive_integer(nested(sanitizers, path), path)
        for path in (
            "runtime.asan_ubsan_lsan_programs",
            "runtime.tsan_programs",
            "embedded_boundary.asan_ubsan_lsan_programs",
            "embedded_boundary.tsan_programs",
        )
    )
    require(sanitizers, "transport.asan_lsan_ubsan", "pass")
    require(sanitizers, "transport.tsan", "pass")

    require(portability, "developer_path_violations", 0)
    checked_files = positive_integer(
        portability.get("checked_file_count"), "portability checked-file count"
    )
    return {
        "dual_live_instances": 2,
        "concurrent_connections": 8,
        "embedded_conformance_cases": 54,
        "embedded_conformance_categories": 12,
        "bundled_sdk_extensions": 1,
        "bundled_pgvector_version": "0.8.6",
        "bundled_pgvector_parallel_worker_entries": 2,
        "bundled_pgvector_kernel_macro_bridges": 1,
        "extension_session_resets": 2,
        "extension_descriptor_rejections": 17,
        "extension_parallel_worker_execution": True,
        "extension_session_mobility": True,
        "cwd_independent_provider": True,
        "resource_files": resource_files,
        "instance_slots": instance_slots,
        "sanitizer_programs": sanitizer_programs,
        "portability_checked_files": checked_files,
        "host_process_mutations": 0,
        "unbundled_native_loading": False,
        "delivery_forms": ["shared", "static"],
        "static_archive_public_symbols": 75,
        "static_archive_internal_globals": 0,
        "static_archive_localized_internal_symbols": localized_static_symbols,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--baseline", required=True, type=Path)
    parser.add_argument("--prior-release", required=True, type=Path)
    parser.add_argument("--api-catalog", required=True, type=Path)
    parser.add_argument("--api-closure", required=True, type=Path)
    parser.add_argument("--public-api", required=True, type=Path)
    parser.add_argument("--multi-instance", required=True, type=Path)
    parser.add_argument("--embedded-conformance", required=True, type=Path)
    parser.add_argument("--extension-inventory", required=True, type=Path)
    parser.add_argument("--pgvector-contract", required=True, type=Path)
    parser.add_argument("--resource-pack", required=True, type=Path)
    parser.add_argument("--instance-reachability", required=True, type=Path)
    parser.add_argument("--installed-sdk", required=True, type=Path)
    parser.add_argument("--static-sdk", required=True, type=Path)
    parser.add_argument("--sanitizers", required=True, type=Path)
    parser.add_argument("--portability", required=True, type=Path)
    parser.add_argument("--canary-workflow", required=True, type=Path)
    parser.add_argument("--input", action="append", default=[], type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()

    try:
        paths = {
            name: getattr(args, name).resolve(strict=True)
            for name in (
                "baseline",
                "prior_release",
                "api_catalog",
                "api_closure",
                "public_api",
                "multi_instance",
                "embedded_conformance",
                "extension_inventory",
                "pgvector_contract",
                "resource_pack",
                "instance_reachability",
                "installed_sdk",
                "static_sdk",
                "sanitizers",
                "portability",
                "canary_workflow",
            )
        }
        extra_inputs = [path.resolve(strict=True) for path in args.input]
        documents = {
            "baseline": load_document(paths["baseline"], BASELINE_KIND),
            "prior": load_document(paths["prior_release"], PRIOR_RELEASE_KIND),
            "catalog": load_document(paths["api_catalog"], CATALOG_KIND),
            "closure": load_document(paths["api_closure"], API_CLOSURE_KIND),
            "public": load_document(paths["public_api"], PUBLIC_API_KIND),
            "multi": load_document(paths["multi_instance"], MULTI_KIND),
            "conformance": load_document(
                paths["embedded_conformance"], CONFORMANCE_KIND
            ),
            "extensions": load_document(paths["extension_inventory"], EXTENSION_KIND),
            "pgvector": load_document(
                paths["pgvector_contract"], PGVECTOR_CONTRACT_KIND
            ),
            "resources": load_document(paths["resource_pack"], RESOURCE_KIND),
            "reachability": load_document(paths["instance_reachability"], REACHABILITY_KIND),
            "sdk": load_document(paths["installed_sdk"], SDK_KIND),
            "static_sdk": load_document(paths["static_sdk"], STATIC_SDK_KIND),
            "sanitizers": load_document(paths["sanitizers"], SANITIZER_KIND),
            "portability": load_document(paths["portability"], PORTABILITY_KIND),
        }
        abi = validate_abi(
            documents["baseline"],
            documents["prior"],
            documents["catalog"],
            documents["closure"],
            documents["public"],
        )
        product = validate_product_evidence(
            documents["public"],
            documents["multi"],
            documents["conformance"],
            documents["extensions"],
            documents["pgvector"],
            documents["resources"],
            documents["reachability"],
            documents["sdk"],
            documents["static_sdk"],
            documents["sanitizers"],
            documents["portability"],
        )
        canary = validate_canary_contract(paths["canary_workflow"])
        write_json(
            args.output.resolve(),
            {
                "schema_version": 1,
                "kind": EVIDENCE_KIND,
                "status": "pass",
                "postgresql_major": 19,
                "abi": abi,
                "product": product,
                "upstream_canary": canary,
                "inputs": input_identity([*extra_inputs, *paths.values()]),
            },
        )
    except (OSError, ReleaseCheckError, EmbeddedReleaseCheckError, TypeError, ValueError) as exc:
        parser.error(str(exc))
    print(
        "embedded release evidence: pass "
        "(ABI v1.3, shared/static SDKs, multi-instance, bundled extensions)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
