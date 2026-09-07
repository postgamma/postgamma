#!/usr/bin/env python3
"""Prove current compatibility with the frozen C API ABI v1.1 surface.

The newest milestone owns exact-surface validation.  This gate intentionally
accepts additive functions, records, numerics, and capabilities while requiring
every v1.1 contract element to remain present and unchanged.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import Any

from check_embedded_lifecycle import input_identity, write_json


RELEASE_KIND = "postgamma.c-abi-release-baseline"
CORE_KIND = "postgamma.core-abi-baseline"
CATALOG_KIND = "postgamma.public-api-ast-catalog"
PROMOTED_CAPABILITIES = {
    "multiple live instances": "PGM_CAP_MULTIPLE_INSTANCES",
}


class ReleaseCheckError(RuntimeError):
    """The public C API release evidence or frozen ABI is incomplete."""


def reject_duplicate_keys(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise ReleaseCheckError(f"duplicate JSON key {key!r}")
        result[key] = value
    return result


def load_document(path: Path, expected_kind: str | None = None) -> dict[str, Any]:
    try:
        document = json.loads(
            path.read_text(encoding="utf-8"),
            object_pairs_hook=reject_duplicate_keys,
        )
    except (OSError, json.JSONDecodeError) as exc:
        raise ReleaseCheckError(f"cannot read {path}: {exc}") from exc
    if not isinstance(document, dict) or document.get("schema_version") != 1:
        raise ReleaseCheckError(f"{path} has an unsupported schema")
    if expected_kind is not None and document.get("kind") != expected_kind:
        raise ReleaseCheckError(f"{path} has unexpected kind {document.get('kind')!r}")
    return document


def unique_entries(entries: Any, key: str, description: str) -> dict[str, Any]:
    if not isinstance(entries, list):
        raise ReleaseCheckError(f"{description} must be an array")
    result: dict[str, Any] = {}
    for entry in entries:
        if not isinstance(entry, dict) or not isinstance(entry.get(key), str):
            raise ReleaseCheckError(f"invalid {description} entry")
        name = entry[key]
        if not name or name in result:
            raise ReleaseCheckError(f"duplicate or empty {description} {name!r}")
        result[name] = entry
    return result


def parse_named_paths(values: list[str]) -> dict[str, Path]:
    raw_paths: dict[str, str] = {}
    for value in values:
        name, separator, raw_path = value.partition("=")
        if not separator or not name or not raw_path or name in raw_paths:
            raise ReleaseCheckError(f"invalid named evidence argument {value!r}")
        raw_paths[name] = raw_path
    return {
        name: Path(raw_path).resolve(strict=True)
        for name, raw_path in raw_paths.items()
    }


def nested(document: dict[str, Any], path: str) -> Any:
    current: Any = document
    for component in path.split("."):
        if not isinstance(current, dict) or component not in current:
            raise ReleaseCheckError(f"evidence field {path!r} is missing")
        current = current[component]
    return current


def require(document: dict[str, Any], path: str, expected: Any) -> None:
    observed = nested(document, path)
    if observed != expected:
        raise ReleaseCheckError(
            f"evidence field {path!r} is {observed!r}, expected {expected!r}"
        )


def normalize_fields(record: dict[str, Any]) -> list[dict[str, Any]]:
    fields = record.get("fields")
    if not isinstance(fields, list):
        raise ReleaseCheckError(f"record {record.get('name')!r} has invalid fields")
    normalized: list[dict[str, Any]] = []
    for field in fields:
        if not isinstance(field, dict):
            raise ReleaseCheckError(
                f"record {record.get('name')!r} has a non-object field"
            )
        normalized.append(
            {
                "name": field.get("name"),
                "canonical_type": field.get("canonical_type"),
                "offset": field.get("offset"),
            }
        )
    return normalized


def validate_exported_symbols(
    released_symbols: set[str], dynamic_symbols: Any, released_count: Any
) -> int:
    if (
        not isinstance(dynamic_symbols, list)
        or not released_symbols <= set(dynamic_symbols)
        or len(released_symbols) != released_count
    ):
        raise ReleaseCheckError("shared-library symbol allowlist changed")
    return len(dynamic_symbols)


def validate_release_abi(
    release: dict[str, Any],
    core: dict[str, Any],
    catalog: dict[str, Any],
    api_closure: dict[str, Any],
    public_api: dict[str, Any],
    installed_sdk: dict[str, Any],
) -> dict[str, Any]:
    release_abi = release.get("abi")
    core_contract = release.get("core_baseline")
    core_abi = core.get("abi")
    if not all(
        isinstance(value, dict)
        for value in (release_abi, core_contract, core_abi)
    ):
        raise ReleaseCheckError("ABI release metadata is incomplete")
    expected_release_abi = {
        "major": 1,
        "minor": 1,
        "encoded_version": 65537,
        "state": "frozen",
        "release_milestone": "sdk-release",
        "delivered_through": "arrow-management",
        "symbol_version_node": "POSTGAMMA_1.0",
        "evolution": "additive-within-major",
    }
    if release_abi != expected_release_abi:
        raise ReleaseCheckError("ABI v1.1 release metadata changed")
    if (
        core_abi.get("major") != 1
        or core_abi.get("minor") != 0
        or core_abi.get("encoded_version") != 65536
        or core_abi.get("state") != "frozen"
        or core_abi.get("frozen_through") != "runtime-events"
    ):
        raise ReleaseCheckError("core ABI v1.0 does not match its frozen baseline")

    core_functions = unique_entries(core.get("functions"), "name", "core function")
    current_functions = unique_entries(
        catalog.get("functions"), "name", "current function"
    )
    additive_functions = unique_entries(
        release.get("additive_functions"), "name", "additive function"
    )
    if len(core_functions) != core_contract.get("function_count"):
        raise ReleaseCheckError("core function count differs from release baseline")
    released_functions = set(core_functions) | set(additive_functions)
    if not released_functions <= set(current_functions):
        raise ReleaseCheckError("the current ABI removed an ABI v1.1 function")
    for name, baseline in {**core_functions, **additive_functions}.items():
        if current_functions[name].get("function_type") != baseline.get("function_type"):
            raise ReleaseCheckError(f"released function signature changed: {name}")

    current_records = unique_entries(
        catalog.get("records"), "name", "current record"
    )
    core_records = unique_entries(core.get("records"), "name", "core record")
    additive_records = unique_entries(
        release.get("additive_complete_records"), "name", "additive record"
    )
    current_complete = {
        name: record
        for name, record in current_records.items()
        if record.get("complete") is True
    }
    if len(core_records) != core_contract.get("complete_record_count"):
        raise ReleaseCheckError("core complete-record count changed")
    released_complete = set(core_records) | set(additive_records)
    if not released_complete <= set(current_complete):
        raise ReleaseCheckError("the current ABI removed an ABI v1.1 record")
    for name, baseline in additive_records.items():
        current = current_complete[name]
        if (
            current.get("size") != baseline.get("size")
            or current.get("alignment") != baseline.get("alignment")
            or normalize_fields(current) != normalize_fields(baseline)
        ):
            raise ReleaseCheckError(f"additive record layout changed: {name}")

    core_opaque = core.get("opaque_records")
    additive_opaque = release.get("additive_opaque_records")
    if not isinstance(core_opaque, list) or not isinstance(additive_opaque, list):
        raise ReleaseCheckError("opaque record baselines are invalid")
    if len(core_opaque) != core_contract.get("opaque_record_count"):
        raise ReleaseCheckError("core opaque-record count changed")
    current_opaque = {
        name
        for name, record in current_records.items()
        if record.get("complete") is False
    }
    released_opaque = set(core_opaque) | set(additive_opaque)
    if not released_opaque <= current_opaque:
        raise ReleaseCheckError("the current ABI removed an ABI v1.1 opaque record")

    core_numerics = core.get("numeric_values")
    current_numerics = nested(api_closure, "numeric_registry.values")
    overrides = release.get("numeric_overrides")
    if not isinstance(core_numerics, dict) or not isinstance(overrides, dict):
        raise ReleaseCheckError("numeric ABI baselines are invalid")
    if len(core_numerics) != core_contract.get("numeric_count"):
        raise ReleaseCheckError("core numeric count changed")
    expected_numerics = {**core_numerics, **overrides}
    version_names = {"PGM_ABI_VERSION", "PGM_ABI_VERSION_MINOR"}
    for name, value in expected_numerics.items():
        if name not in version_names and current_numerics.get(name) != value:
            raise ReleaseCheckError(f"ABI v1.1 numeric value changed: {name}")
    current_major = current_numerics.get("PGM_ABI_VERSION_MAJOR")
    current_minor = current_numerics.get("PGM_ABI_VERSION_MINOR")
    current_version = current_numerics.get("PGM_ABI_VERSION")
    if (
        current_major != release_abi["major"]
        or not isinstance(current_minor, int)
        or current_minor < release_abi["minor"]
        or current_version != (current_major << 16) | current_minor
    ):
        raise ReleaseCheckError("the current ABI version predates ABI v1.1")

    expected_symbols = released_functions
    dynamic_symbols = nested(public_api, "dynamic_symbols.exported_symbols")
    current_symbol_count = validate_exported_symbols(
        expected_symbols,
        dynamic_symbols,
        release.get("exported_symbol_count"),
    )
    require(
        public_api,
        "dynamic_symbols.version_node",
        release_abi["symbol_version_node"],
    )
    require(api_closure, "core_abi_baseline.breaking_changes", 0)
    require(api_closure, "core_abi_baseline.status", "pass")
    closure_version = nested(api_closure, "abi.encoded_version")
    if closure_version < release_abi["encoded_version"]:
        raise ReleaseCheckError("the current API closure predates ABI v1.1")
    required_capabilities = nested(release, "capabilities.advertised_value")
    observed_capabilities = int(nested(installed_sdk, "marker.capabilities"))
    if observed_capabilities & required_capabilities != required_capabilities:
        raise ReleaseCheckError("the current SDK removed an ABI v1.1 capability")
    return {
        "major": release_abi["major"],
        "minor": release_abi["minor"],
        "encoded_version": release_abi["encoded_version"],
        "core_abi_compatible": True,
        "breaking_changes": 0,
        "core_function_count": len(core_functions),
        "additive_function_count": len(additive_functions),
        "released_exported_symbol_count": len(expected_symbols),
        "current_exported_symbol_count": current_symbol_count,
        "core_complete_record_count": len(core_records),
        "additive_complete_record_count": len(additive_records),
        "core_opaque_record_count": len(core_opaque),
        "additive_opaque_record_count": len(additive_opaque),
        "released_numeric_count": len(expected_numerics),
        "current_numeric_count": len(current_numerics),
        "advertised_capabilities": nested(
            release, "capabilities.advertised_value"
        ),
        "symbol_version_node": release_abi["symbol_version_node"],
    }


def validate_evidence_contract(
    release: dict[str, Any], evidence_paths: dict[str, Path]
) -> dict[str, dict[str, Any]]:
    requirements = unique_entries(
        release.get("required_evidence"), "name", "evidence requirement"
    )
    if set(requirements) != set(evidence_paths):
        raise ReleaseCheckError(
            "release evidence set mismatch: "
            f"missing={sorted(set(requirements) - set(evidence_paths))}, "
            f"unexpected={sorted(set(evidence_paths) - set(requirements))}"
        )
    documents: dict[str, dict[str, Any]] = {}
    for name, requirement in requirements.items():
        document = load_document(evidence_paths[name], requirement.get("kind"))
        field = requirement.get("field")
        if not isinstance(field, str):
            raise ReleaseCheckError(f"evidence requirement {name} has no field")
        require(document, field, requirement.get("value"))
        documents[name] = document
    return documents


def zero_host_hazards(report: dict[str, Any], prefix: str) -> None:
    for name in (
        "process_creation_calls",
        "host_signal_delivery_calls",
        "network_endpoint_calls",
        "process_global_state_calls",
    ):
        require(report, f"{prefix}.{name}", 0)


def zero_embedded_syscalls(report: dict[str, Any], prefix: str) -> None:
    for name in (
        "process_creation_calls",
        "host_signal_delivery_calls",
        "network_endpoint_calls",
        "process_global_cwd_calls",
        "process_global_umask_calls",
    ):
        require(report, f"{prefix}.{name}", 0)


def validate_exit_criteria(
    evidence: dict[str, dict[str, Any]]
) -> list[dict[str, Any]]:
    public = evidence["public_api"]
    ordered = evidence["ordered_results"]
    streaming = evidence["bounded_streaming"]
    copy = evidence["copy"]
    events = evidence["events"]
    arrow_management = evidence["arrow_management"]
    executor = evidence["session_executor"]
    differential = evidence["executor_differential"]
    transport = evidence["memory_transport"]

    require(public, "frontend_execution.caller_driven", True)
    require(public, "frontend_execution.request_thread_creation_sites", 0)
    require(ordered, "inferred_and_explicit_parameter_oids", True)
    require(ordered, "text_and_binary_parameters_and_results", True)
    if nested(ordered, "prepared_execution_count") < 3:
        raise ReleaseCheckError("prepared-statement matrix is incomplete")
    require(ordered, "socket_libpq_matches_embedded_public_api", True)
    if nested(ordered, "ordered_result_count") < 6:
        raise ReleaseCheckError("ordered result matrix is incomplete")
    if nested(streaming, "large_stream_rows") < 10_000_000:
        raise ReleaseCheckError("large-result gate streamed fewer than ten million rows")
    if nested(streaming, "rss_delta_bytes") > nested(
        streaming, "rss_allowance_bytes"
    ):
        raise ReleaseCheckError("streaming memory exceeded its hard allowance")
    require(streaming, "bounded_result_and_value_limits", True)
    require(streaming, "terminal_result_observed_for_every_sequence", True)
    require(copy, "partial_duplex_io", True)
    require(copy, "socket_libpq_matches_embedded_public_api", True)
    require(copy, "deterministic_concurrent_owner_conflict", True)
    require(copy, "copy_lifecycle_matrix", True)
    require(copy, "connection_reusable_after_every_retained_request", True)
    require(ordered, "connection_reusable_after_cancel", True)
    require(ordered, "connection_reusable_after_error", True)
    require(events, "normal.marker.exact_once", "true")
    require(events, "normal.marker.callback_host_thread", "true")
    require(events, "normal.marker.request_notice_override", "true")
    require(events, "normal.marker.diagnostics", "17")
    require(events, "normal.marker.reentrant_status", "14")
    require(executor, "marker.connections", "1000")
    require(executor, "marker.executor_workers", "4")
    require(executor, "marker.request_threads", "0")
    require(executor, "marker.parallel_query", "true")
    require(executor, "marker.pinning_cliff", "true")
    require(executor, "marker.queued_at_cliff", "true")
    require(events, "event_routing.ready_queue_routing", "o1")
    require(events, "event_routing.connection_registry_scans_on_pump", 0)
    require(events, "scale.public_waitables_per_connection", 0)
    require(events, "scale.public_waitables_per_instance", 1)
    require(public, "marker.holdable_cursor_migration", "true")
    require(executor, "marker.holder_progress", "true")
    if set(nested(differential, "runs")) != {"dedicated", "pooled"}:
        raise ReleaseCheckError("executor differential did not cover both providers")
    for name in ("primitive_mapping", "nulls", "metadata", "fallback",
                 "result_independent", "release", "no_arrow_runtime"):
        require(arrow_management, f"arrow.marker.{name}", "true")
    for name in ("checkpoint", "caller_driven", "waitable", "lsn_advanced",
                 "close_gate"):
        require(arrow_management, f"management.marker.{name}", "true")
    require(arrow_management, "management.marker.hidden_connections", "0")
    require(arrow_management, "management.marker.subprocesses", "0")
    require(public, "public_header.postgres_headers", 0)
    require(public, "public_header.postgres_types", 0)
    require(
        arrow_management, "arrow.boundary.adapter_header_is_optional", True
    )
    require(arrow_management, "arrow.boundary.arrow_runtime_dependencies", 0)
    for report, prefix in (
        (public, "host_safety"),
        (ordered, "candidate_host_safety"),
        (streaming, "candidate_host_safety"),
        (copy, "candidate_host_safety"),
        (events, "host_safety"),
        (arrow_management, "arrow.host_safety"),
        (arrow_management, "management.host_safety"),
        (evidence["cluster_creation"], "host_safety"),
        (evidence["installed_sdk"], "host_safety"),
    ):
        zero_host_hazards(report, prefix)
    for report, prefix in (
        (evidence["lifecycle"], "syscalls"),
        (evidence["query"], "syscalls"),
        (evidence["crash_recovery"], "reopen_syscalls"),
    ):
        zero_embedded_syscalls(report, prefix)
    if nested(ordered, "candidate_process_id") == nested(
        ordered, "reference_process_id"
    ):
        raise ReleaseCheckError("reference and candidate trace scopes overlap")
    require(transport, "metrics.asan_lsan_ubsan", "pass")
    require(transport, "metrics.tsan", "pass")
    require(evidence["sanitizers"], "status", "pass")
    require(evidence["sanitizers"], "runtime.asan_ubsan_lsan_programs", 5)
    require(evidence["sanitizers"], "runtime.tsan_programs", 5)
    require(
        evidence["sanitizers"],
        "embedded_boundary.asan_ubsan_lsan_programs",
        3,
    )
    require(evidence["sanitizers"], "embedded_boundary.tsan_programs", 3)
    require(evidence["crash_recovery"], "status", "pass")
    require(evidence["lifecycle"], "status", "pass")
    require(evidence["soak"], "status", "pass")
    require(evidence["portability"], "status", "pass")
    require(evidence["portability"], "developer_path_violations", 0)
    require(evidence["installed_sdk"], "consumer.repository_include_directories", 0)
    require(evidence["thread_isolation"], "telemetry.role_process_launches", 0)
    require(evidence["thread_isolation"], "telemetry.role_threads_active", 0)
    require(evidence["soak"], "run.iterations", 1000)
    require(evidence["soak"], "run.marker.resources_restored", "true")

    descriptions = [
        "sync and async APIs share one caller-driven private-libpq engine",
        "prepared statements cover explicit and inferred text/binary parameters",
        "ordered PostgreSQL results are preserved",
        "ten million tuple rows stream within the measured memory bound",
        "COPY IN and COPY OUT use bounded partial duplex I/O",
        "deterministic COPY interleavings complete without self-deadlock",
        "cancel and error paths leave connections reusable or explicitly retired",
        "diagnostics, notices, notifications, and results preserve routed identity",
        "callbacks run only on host progress threads and reject reentry",
        "one thousand connections execute on four workers without request threads",
        "pinning cliffs and scheduler pressure are observable",
        "one O(1) instance waitable routes events without public per-connection FDs",
        "COPY result ownership and concurrent access are exclusive",
        "session execution migration, holder progress, pinning, and provider differential remain green",
        "optional Arrow export passes type, null, metadata, and lifetime checks",
        "checkpoint management runs in process without a hidden connection",
        "public headers contain no PostgreSQL or mandatory Arrow dependency",
        "ABI v1.1 matches the frozen v1.0 core plus the reviewed additive surface",
        "reference Oracle activity is outside every candidate host-safety trace",
        "sanitizer, recovery, lifecycle, soak, and portability gates pass",
        "unsupported ABI v1.1 and future product capabilities are explicit",
    ]
    return [
        {
            "id": index,
            "status": "pass",
            "description": description,
        }
        for index, description in enumerate(descriptions, start=1)
    ]


def validate_documentation(
    release: dict[str, Any],
    readme: Path,
    api_closure: dict[str, Any],
    installed_sdk: dict[str, Any],
) -> dict[str, list[str]]:
    content = readme.read_text(encoding="utf-8").casefold()
    requirements = release.get("documented_unsupported")
    if not isinstance(requirements, list) or not all(
        isinstance(value, str) and value for value in requirements
    ):
        raise ReleaseCheckError("documented unsupported capability list is invalid")
    missing = [value for value in requirements if value.casefold() not in content]
    if missing:
        raise ReleaseCheckError(
            "README omits capability boundaries: " + ", ".join(missing)
        )
    numerics = nested(api_closure, "numeric_registry.values")
    observed = int(nested(installed_sdk, "marker.capabilities"))
    unsupported: list[str] = []
    promoted: list[str] = []
    for requirement in requirements:
        capability_name = PROMOTED_CAPABILITIES.get(requirement)
        capability_value = (
            numerics.get(capability_name)
            if capability_name is not None and isinstance(numerics, dict)
            else None
        )
        if (
            isinstance(capability_value, int)
            and not isinstance(capability_value, bool)
            and observed & capability_value
        ):
            promoted.append(requirement)
        else:
            unsupported.append(requirement)
    return {
        "unsupported": unsupported,
        "promoted": promoted,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", required=True, type=Path)
    parser.add_argument("--release-baseline", required=True, type=Path)
    parser.add_argument("--core-baseline", required=True, type=Path)
    parser.add_argument("--api-catalog", required=True, type=Path)
    parser.add_argument("--readme", required=True, type=Path)
    parser.add_argument("--evidence", action="append", default=[])
    parser.add_argument("--input", action="append", default=[], type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()

    args.output.resolve().parent.mkdir(parents=True, exist_ok=True)
    args.output.resolve().unlink(missing_ok=True)
    try:
        root = args.root.resolve(strict=True)
        release_path = args.release_baseline.resolve(strict=True)
        core_path = args.core_baseline.resolve(strict=True)
        catalog_path = args.api_catalog.resolve(strict=True)
        readme = args.readme.resolve(strict=True)
        inputs = [path.resolve(strict=True) for path in args.input]
        release = load_document(release_path, RELEASE_KIND)
        core = load_document(core_path, CORE_KIND)
        catalog = load_document(catalog_path, CATALOG_KIND)
        expected_core_path = (root / nested(release, "core_baseline.path")).resolve()
        if core_path != expected_core_path:
            raise ReleaseCheckError("release baseline references a different core ABI")
        evidence_paths = parse_named_paths(args.evidence)
        evidence = validate_evidence_contract(release, evidence_paths)
        abi = validate_release_abi(
            release,
            core,
            catalog,
            evidence["api_contract"],
            evidence["public_api"],
            evidence["installed_sdk"],
        )
        documented = validate_documentation(
            release,
            readme,
            evidence["api_contract"],
            evidence["installed_sdk"],
        )
        criteria = validate_exit_criteria(evidence)
        document = {
            "schema_version": 1,
            "kind": "postgamma.c-api-release",
            "status": "pass",
            "postgresql_major": 19,
            "milestone": "sdk-release",
            "c_api_ready": True,
            "product_ready": False,
            "product_ready_reason": (
                "logical management backup/migration, embedded release multi-instance and extension "
                "hardening, and Python language bindings remain separate capabilities."
            ),
            "abi": abi,
            "exit_criteria": criteria,
            "exit_criteria_passed": len(criteria),
            "documented_unsupported": documented["unsupported"],
            "documented_promoted": documented["promoted"],
            "evidence": {
                name: {
                    "path": str(path),
                    "kind": evidence[name]["kind"],
                }
                for name, path in sorted(evidence_paths.items())
            },
            "inputs": input_identity(
                [
                    *inputs,
                    release_path,
                    core_path,
                    catalog_path,
                    readme,
                    *evidence_paths.values(),
                ]
            ),
        }
        write_json(args.output.resolve(), document)
    except (OSError, ReleaseCheckError) as exc:
        parser.error(str(exc))
    print(
        "public C API release evidence: pass "
        "(21/21 criteria, ABI v1.1 additive over frozen core v1.0)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
