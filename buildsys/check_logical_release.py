#!/usr/bin/env python3
"""Freeze ABI v1.2 and close the logical-management evidence graph.

After ABI v1.3, this historical gate proves the frozen v1.2 lower bound and
baseline self-consistency.  Exact equality with the current product catalog is
owned by the newest release gate, currently check_embedded_release.py.
"""

from __future__ import annotations

import argparse
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


BASELINE_KIND = "postgamma.c-abi-release-baseline"
PRIOR_KIND = "postgamma.c-abi-release-baseline"
CORE_KIND = "postgamma.core-abi-baseline"
CATALOG_KIND = "postgamma.public-api-ast-catalog"
API_CLOSURE_KIND = "postgamma.c-api-closure"
PUBLIC_API_KIND = "postgamma.public-c-api"
LOGICAL_KIND = "postgamma.logical-management"
PROCESS_ASSUMPTION_KIND = "postgamma.embedded-process-assumptions"
LOGICAL_TOOL_LINK_KIND = "postgamma.private-frontend-tool-link"

LOGICAL_PROCESS_PROVIDER_IDS = frozenset(
    {
        "embedded.logical-tool.abort-provider",
        "embedded.logical-tool.exit-provider",
        "embedded.logical-tool.immediate-exit-provider",
        "embedded.logical-tool.chdir-provider",
        "embedded.logical-tool.setlocale-provider",
        "embedded.logical-tool.setenv-provider",
        "embedded.logical-tool.unsetenv-provider",
        "embedded.logical-tool.umask-provider",
        "embedded.logical-tool.system-provider",
        "embedded.logical-tool.popen-provider",
        "embedded.logical-tool.fork-provider",
        "embedded.logical-tool.kill-provider",
        "embedded.logical-tool.raise-provider",
    }
)

FORBIDDEN_NAMESPACED_LIBPQ_SYMBOLS = frozenset(
    {
        "PQconnectdb",
        "PQconnectdbParams",
        "PQconnectStart",
        "PQconnectStartParams",
        "PQconnectPoll",
        "PQsetdbLogin",
        "PQping",
        "PQpingParams",
        "PQcancel",
        "PQcancelCreate",
        "PQcancelStart",
        "PQcancelPoll",
        "PQcancelFinish",
        "PQrequestCancel",
        "PQgetCancel",
        "PQfreeCancel",
    }
)


class LogicalReleaseCheckError(RuntimeError):
    """The ABI v1.2 baseline or logical-management graph is incomplete."""


def exact_abi_metadata(baseline: dict[str, Any]) -> dict[str, Any]:
    expected = {
        "major": 1,
        "minor": 2,
        "encoded_version": 65538,
        "state": "frozen",
        "release_milestone": "logical-management",
        "delivered_through": "logical-management",
        "symbol_version_node": "POSTGAMMA_1.0",
        "evolution": "additive-within-major",
    }
    if baseline.get("abi") != expected:
        raise LogicalReleaseCheckError("ABI v1.2 release metadata changed")
    return expected


def validate_functions(
    baseline: dict[str, Any],
    prior: dict[str, Any],
    core: dict[str, Any],
    catalog: dict[str, Any],
) -> dict[str, int]:
    core_functions = unique_entries(core.get("functions"), "name", "core function")
    prior_additions = unique_entries(
        prior.get("additive_functions"), "name", "ABI v1.1 function"
    )
    additions = unique_entries(
        baseline.get("additive_functions"), "name", "ABI v1.2 function"
    )
    current = unique_entries(catalog.get("functions"), "name", "current function")
    prior_functions = {**core_functions, **prior_additions}
    if len(prior_functions) != nested(baseline, "prior_baseline.function_count"):
        raise LogicalReleaseCheckError("ABI v1.1 function count changed")
    if set(prior_functions) & set(additions):
        raise LogicalReleaseCheckError("ABI v1.2 redefines a prior function")
    expected = {**prior_functions, **additions}
    if not set(expected) <= set(current):
        raise LogicalReleaseCheckError(
            "a released ABI v1.2 function is missing"
        )
    for name, frozen in expected.items():
        if current[name].get("function_type") != frozen.get("function_type"):
            raise LogicalReleaseCheckError(f"released function changed: {name}")
    if len(expected) != baseline.get("exported_symbol_count"):
        raise LogicalReleaseCheckError(
            "ABI v1.2 declared function count is internally inconsistent"
        )
    return {
        "prior": len(prior_functions),
        "additive": len(additions),
        "current": len(current),
    }


def require_record_layout(
    name: str, frozen: dict[str, Any], current: dict[str, Any]
) -> None:
    if (
        current.get("size") != frozen.get("size")
        or current.get("alignment") != frozen.get("alignment")
        or normalize_fields(current) != normalize_fields(frozen)
    ):
        raise LogicalReleaseCheckError(f"released record layout changed: {name}")


def validate_records(
    baseline: dict[str, Any],
    prior: dict[str, Any],
    core: dict[str, Any],
    catalog: dict[str, Any],
) -> dict[str, int]:
    current_records = unique_entries(
        catalog.get("records"), "name", "current record"
    )
    current_complete = {
        name: record
        for name, record in current_records.items()
        if record.get("complete") is True
    }
    current_opaque = {
        name
        for name, record in current_records.items()
        if record.get("complete") is False
    }
    core_records = unique_entries(core.get("records"), "name", "core record")
    prior_records = unique_entries(
        prior.get("additive_complete_records"),
        "name",
        "ABI v1.1 record",
    )
    additions = unique_entries(
        baseline.get("additive_complete_records"),
        "name",
        "ABI v1.2 record",
    )
    prior_complete_names = set(core_records) | set(prior_records)
    if len(prior_complete_names) != nested(
        baseline, "prior_baseline.complete_record_count"
    ):
        raise LogicalReleaseCheckError("ABI v1.1 complete-record count changed")
    if prior_complete_names & set(additions):
        raise LogicalReleaseCheckError("ABI v1.2 redefines a prior record")
    expected_complete = prior_complete_names | set(additions)
    if not expected_complete <= set(current_complete):
        raise LogicalReleaseCheckError(
            "a released ABI v1.2 complete record is missing"
        )
    for name, frozen in prior_records.items():
        require_record_layout(name, frozen, current_complete[name])
    for name, frozen in additions.items():
        require_record_layout(name, frozen, current_complete[name])

    core_opaque = core.get("opaque_records")
    prior_opaque = prior.get("additive_opaque_records")
    additive_opaque = baseline.get("additive_opaque_records")
    if not all(
        isinstance(value, list)
        for value in (core_opaque, prior_opaque, additive_opaque)
    ):
        raise LogicalReleaseCheckError("opaque record baselines are invalid")
    prior_opaque_names = set(core_opaque) | set(prior_opaque)
    if len(prior_opaque_names) != nested(
        baseline, "prior_baseline.opaque_record_count"
    ):
        raise LogicalReleaseCheckError("ABI v1.1 opaque-record count changed")
    expected_opaque = prior_opaque_names | set(additive_opaque)
    if not expected_opaque <= current_opaque:
        raise LogicalReleaseCheckError("a released ABI v1.2 opaque record is missing")
    if (
        len(expected_complete) != baseline.get("complete_record_count")
        or len(expected_opaque) != baseline.get("opaque_record_count")
    ):
        raise LogicalReleaseCheckError(
            "ABI v1.2 declared record counts are internally inconsistent"
        )
    return {
        "prior_complete": len(prior_complete_names),
        "additive_complete": len(additions),
        "current_complete": len(current_complete),
        "current_opaque": len(current_opaque),
    }


def validate_numerics(
    baseline: dict[str, Any],
    prior: dict[str, Any],
    core: dict[str, Any],
    api_closure: dict[str, Any],
) -> tuple[dict[str, int], dict[str, int]]:
    core_values = core.get("numeric_values")
    prior_overrides = prior.get("numeric_overrides")
    additions = baseline.get("numeric_additions")
    overrides = baseline.get("numeric_overrides")
    if not all(
        isinstance(value, dict)
        for value in (core_values, prior_overrides, additions, overrides)
    ):
        raise LogicalReleaseCheckError("numeric ABI baselines are invalid")
    prior_values = {**core_values, **prior_overrides}
    if len(prior_values) != nested(baseline, "prior_baseline.numeric_count"):
        raise LogicalReleaseCheckError("ABI v1.1 numeric count changed")
    if set(prior_values) & set(additions):
        raise LogicalReleaseCheckError("ABI v1.2 redefines a prior numeric value")
    expected = {**prior_values, **additions, **overrides}
    current = nested(api_closure, "numeric_registry.values")
    version_names = {
        "PGM_ABI_VERSION",
        "PGM_ABI_VERSION_MAJOR",
        "PGM_ABI_VERSION_MINOR",
    }
    for name, value in expected.items():
        if name not in version_names and current.get(name) != value:
            raise LogicalReleaseCheckError(f"released numeric value changed: {name}")
    if len(expected) != baseline.get("numeric_count"):
        raise LogicalReleaseCheckError(
            "ABI v1.2 declared numeric count is internally inconsistent"
        )
    return additions, current


def validate_capabilities(
    baseline: dict[str, Any], numerics: dict[str, int], public_api: dict[str, Any]
) -> int:
    capabilities = baseline.get("capabilities")
    if not isinstance(capabilities, dict):
        raise LogicalReleaseCheckError("ABI v1.2 capability baseline is missing")
    advertised = capabilities.get("advertised")
    reserved = capabilities.get("reserved_unadvertised")
    if not isinstance(advertised, list) or not isinstance(reserved, list):
        raise LogicalReleaseCheckError("ABI v1.2 capability lists are invalid")
    if set(advertised) & set(reserved):
        raise LogicalReleaseCheckError("a capability is both advertised and reserved")
    try:
        advertised_value = sum(numerics[name] for name in advertised)
        permanently_reserved = set(reserved) - {"PGM_CAP_MULTIPLE_INSTANCES"}
        reserved_value = sum(numerics[name] for name in permanently_reserved)
    except KeyError as exc:
        raise LogicalReleaseCheckError(
            f"capability baseline references unknown value {exc.args[0]}"
        ) from exc
    if advertised_value != capabilities.get("advertised_value"):
        raise LogicalReleaseCheckError("advertised capability mask changed")
    observed = int(nested(public_api, "marker.capabilities"))
    if (observed & advertised_value) != advertised_value or observed & reserved_value:
        raise LogicalReleaseCheckError("runtime capability mask violates ABI v1.2")
    return advertised_value


def validate_process_assumptions(report: dict[str, Any]) -> dict[str, int]:
    require(report, "status", "pass")
    records = unique_entries(
        report.get("records"), "id", "process-assumption record"
    )
    missing = sorted(LOGICAL_PROCESS_PROVIDER_IDS - set(records))
    if missing:
        raise LogicalReleaseCheckError(
            "logical management process providers are not inventoried: " + ", ".join(missing)
        )
    for identifier in LOGICAL_PROCESS_PROVIDER_IDS:
        record = records[identifier]
        if (
            record.get("classification") != "required_for_logical_management"
            or record.get("actual_matches") != 1
            or record.get("status") != "classified"
        ):
            raise LogicalReleaseCheckError(
                f"logical management process provider classification changed: {identifier}"
            )
    project_calls = report.get("project_process_call_count")
    if not isinstance(project_calls, int) or isinstance(project_calls, bool):
        raise LogicalReleaseCheckError("project process-call count is invalid")
    return {
        "logical_management_provider_count": len(LOGICAL_PROCESS_PROVIDER_IDS),
        "project_process_call_count": project_calls,
    }


def validate_logical_tool_namespace(
    baseline: dict[str, Any], link_report: dict[str, Any]
) -> int:
    expected = nested(baseline, "private_logical_tool.namespaced_libpq_symbols")
    observed = link_report.get("namespaced_symbols")
    if (
        link_report.get("kind") != LOGICAL_TOOL_LINK_KIND
        or not isinstance(expected, list)
        or not expected
        or len(expected) != len(set(expected))
        or expected != sorted(expected)
        or not isinstance(observed, dict)
        or set(observed) != set(expected)
        or any(
            target != "postgamma_private_libpq_symbol_" + source
            for source, target in observed.items()
        )
    ):
        raise LogicalReleaseCheckError(
            "private logical-tool libpq namespace differs from its reviewed baseline"
        )
    forbidden = sorted(set(expected) & FORBIDDEN_NAMESPACED_LIBPQ_SYMBOLS)
    if forbidden:
        raise LogicalReleaseCheckError(
            "host-sensitive libpq entry points reached the private namespace: "
            + ", ".join(forbidden)
        )
    return len(expected)


def validate_evidence(
    baseline: dict[str, Any],
    catalog: dict[str, Any],
    api_closure: dict[str, Any],
    public_api: dict[str, Any],
    logical: dict[str, Any],
) -> dict[str, Any]:
    abi = exact_abi_metadata(baseline)
    require(api_closure, "status", "pass")
    current_abi = nested(api_closure, "abi.encoded_version")
    if current_abi < abi["encoded_version"] or current_abi >> 16 != 1:
        raise LogicalReleaseCheckError("current ABI no longer extends ABI v1.2")
    require(api_closure, "core_abi_baseline.breaking_changes", 0)
    require(public_api, "status", "pass")
    if int(nested(public_api, "marker.abi")) != current_abi:
        raise LogicalReleaseCheckError("public runtime ABI differs from its catalog")
    require(
        public_api,
        "dynamic_symbols.version_node",
        abi["symbol_version_node"],
    )
    require(logical, "status", "pass")
    require(logical, "public_api.archive_exceeds_channel_capacity", True)
    require(logical, "public_api.host_diagnostic_leak_count", 0)
    require(logical, "public_api.stream_sentinel_filesystem_opens", 0)
    if nested(logical, "public_api.dump_callback_calls") < 2:
        raise LogicalReleaseCheckError("logical dump did not cross callback boundaries")
    if nested(logical, "public_api.restore_callback_calls") < 2:
        raise LogicalReleaseCheckError(
            "logical restore did not cross callback boundaries"
        )
    if nested(logical, "public_api.sentinel_positive_control_events") < 1:
        raise LogicalReleaseCheckError("filesystem sentinel gate lacks a positive control")
    require(logical, "public_api.marker.failure_retry", "true")
    require(logical, "public_api.marker.cancel_retry", "true")
    require(logical, "public_api.marker.subprocesses", "0")
    require(logical, "public_api.marker.stable_waitable", "true")
    require(logical, "public_api.marker.running_free_cleanup", "true")
    require(
        logical,
        "public_api.marker.callbacks_on_progress_thread",
        "true",
    )
    if logical.get("library") != public_api.get("library"):
        raise LogicalReleaseCheckError(
            "logical evidence did not execute the audited shared library"
        )
    exports = nested(public_api, "dynamic_symbols.exported_symbols")
    functions = unique_entries(catalog.get("functions"), "name", "current function")
    if not isinstance(exports, list) or set(exports) != set(functions):
        raise LogicalReleaseCheckError("public dynamic symbols differ from the AST ABI")
    return {
        "core_abi_compatible": True,
        "breaking_changes": 0,
        "shared_library_executed": True,
        "logical_backup": True,
        "logical_restore": True,
        "maintenance": True,
        "failure_and_cancel_retry": True,
        "host_safe": True,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--baseline", required=True, type=Path)
    parser.add_argument("--prior-baseline", required=True, type=Path)
    parser.add_argument("--core-baseline", required=True, type=Path)
    parser.add_argument("--api-catalog", required=True, type=Path)
    parser.add_argument("--api-closure", required=True, type=Path)
    parser.add_argument("--public-api", required=True, type=Path)
    parser.add_argument("--logical-evidence", required=True, type=Path)
    parser.add_argument("--process-assumptions", required=True, type=Path)
    parser.add_argument("--logical-tool-link", required=True, type=Path)
    parser.add_argument("--input", action="append", default=[], type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()

    try:
        baseline_path = args.baseline.resolve(strict=True)
        prior_path = args.prior_baseline.resolve(strict=True)
        core_path = args.core_baseline.resolve(strict=True)
        catalog_path = args.api_catalog.resolve(strict=True)
        closure_path = args.api_closure.resolve(strict=True)
        public_path = args.public_api.resolve(strict=True)
        logical_path = args.logical_evidence.resolve(strict=True)
        process_path = args.process_assumptions.resolve(strict=True)
        logical_tool_link_path = args.logical_tool_link.resolve(strict=True)
        inputs = [path.resolve(strict=True) for path in args.input]
        baseline = load_document(baseline_path, BASELINE_KIND)
        prior = load_document(prior_path, PRIOR_KIND)
        core = load_document(core_path, CORE_KIND)
        catalog = load_document(catalog_path, CATALOG_KIND)
        api_closure = load_document(closure_path, API_CLOSURE_KIND)
        public_api = load_document(public_path, PUBLIC_API_KIND)
        logical = load_document(logical_path, LOGICAL_KIND)
        process_assumptions = load_document(
            process_path, PROCESS_ASSUMPTION_KIND
        )
        logical_tool_link = load_document(logical_tool_link_path)
        if baseline.get("postgresql_major") != 19:
            raise LogicalReleaseCheckError("ABI v1.2 must target PostgreSQL 19")
        if nested(baseline, "prior_baseline.path") != (
            "manifests/api/c-abi-v1-1.json"
        ):
            raise LogicalReleaseCheckError("ABI v1.2 predecessor path changed")
        function_report = validate_functions(baseline, prior, core, catalog)
        record_report = validate_records(baseline, prior, core, catalog)
        additions, numerics = validate_numerics(
            baseline, prior, core, api_closure
        )
        capability_mask = validate_capabilities(baseline, numerics, public_api)
        evidence = validate_evidence(
            baseline, catalog, api_closure, public_api, logical
        )
        process_report = validate_process_assumptions(process_assumptions)
        namespaced_symbol_count = validate_logical_tool_namespace(
            baseline, logical_tool_link
        )
        document = {
            "schema_version": 1,
            "kind": "postgamma.logical-release",
            "status": "pass",
            "postgresql_major": 19,
            "abi": {
                **baseline["abi"],
                "prior_abi_compatible": True,
                "breaking_changes": 0,
                "functions": function_report,
                "records": record_report,
                "additive_numeric_count": len(additions),
                "numeric_count": len(numerics),
                "advertised_capabilities": capability_mask,
            },
            "evidence": {
                **evidence,
                "process_assumptions": process_report,
                "namespaced_libpq_symbol_count": namespaced_symbol_count,
            },
            "inputs": input_identity(
                [
                    *inputs,
                    baseline_path,
                    prior_path,
                    core_path,
                    catalog_path,
                    closure_path,
                    public_path,
                    logical_path,
                    process_path,
                    logical_tool_link_path,
                ]
            ),
        }
        write_json(args.output.resolve(), document)
    except (OSError, ReleaseCheckError, LogicalReleaseCheckError, ValueError) as exc:
        parser.error(str(exc))
    print(
        "logical-management release evidence: pass "
        "(ABI v1.2, shared-library logical backup/restore, maintenance)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
