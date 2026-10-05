#!/usr/bin/env python3
"""Validate the pinned pgvector source, SQL, state, and linker closure."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import subprocess
from pathlib import Path
from typing import Any

from bundle_embedded_static_modules import (
    global_definitions,
    read_json,
    validate_manifest,
)
from generate_extension_state_plan import parse_policy


EVIDENCE_KIND = "postgamma.pgvector-contract"
FUNCTION_PATTERN = re.compile(
    r"\bCREATE\s+FUNCTION\s+([A-Za-z_][A-Za-z0-9_]*)\s*\("
    r"(?P<body>[^;]*?)\bAS\s+'MODULE_PATHNAME'"
    r"(?:\s*,\s*'([A-Za-z_][A-Za-z0-9_]*)')?[^;]*;",
    re.IGNORECASE | re.DOTALL,
)
PARALLEL_WORKER_PATTERN = re.compile(
    r"\bCreateParallelContext\s*\(\s*\"vector\"\s*,\s*"
    r"\"([A-Za-z_][A-Za-z0-9_]*)\"",
    re.MULTILINE,
)
KERNEL_MACRO_BRIDGES = (
    {
        "source_macro": "AddinShmemInitLock",
        "source_use_count": 2,
        "kernel_state_id": "external:MainLWLockArray",
        "kernel_global": "MainLWLockArray",
        "accessor_macro": "POSTGAMMA_BACKEND_STATE_GLOBAL_MainLWLockArray",
    },
)


class PgvectorContractError(ValueError):
    """The checked-in pgvector integration no longer matches its source."""


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def git(repository: Path, *arguments: str) -> str:
    completed = subprocess.run(
        ["git", "-C", str(repository), *arguments],
        check=False,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    if completed.returncode != 0:
        diagnostic = completed.stderr.strip() or completed.stdout.strip()
        raise PgvectorContractError(f"git {' '.join(arguments)} failed: {diagnostic}")
    return completed.stdout.strip()


def sql_symbols(sql: str) -> list[str]:
    symbols: set[str] = set()
    for match in FUNCTION_PATTERN.finditer(sql):
        sql_name = match.group(1)
        symbols.add(match.group(3) or sql_name)
    if not symbols:
        raise PgvectorContractError("pgvector SQL declares no C functions")
    return sorted(symbols)


def parallel_worker_symbols(source: Path) -> list[str]:
    symbols: set[str] = set()
    for path in sorted((source / "src").glob("*.c")):
        symbols.update(
            PARALLEL_WORKER_PATTERN.findall(path.read_text(encoding="utf-8"))
        )
    if not symbols:
        raise PgvectorContractError("pgvector declares no parallel worker entries")
    return sorted(symbols)


def expected_symbols(sql: str, source: Path) -> dict[str, str]:
    result = {
        "Pg_magic_func": "postgamma_module_pgvector_magic",
        "_PG_init": "postgamma_module_pgvector_initialize",
    }
    for symbol in sql_symbols(sql):
        result[symbol] = f"postgamma_module_pgvector_{symbol}"
        result[f"pg_finfo_{symbol}"] = f"postgamma_module_pgvector_finfo_{symbol}"
    for symbol in parallel_worker_symbols(source):
        result[symbol] = f"postgamma_module_pgvector_{symbol}"
    return result


def control_value(control: str, name: str) -> str:
    match = re.search(
        rf"^\s*{re.escape(name)}\s*=\s*'([^']+)'\s*$",
        control,
        re.MULTILINE,
    )
    if match is None:
        raise PgvectorContractError(f"vector.control does not define {name}")
    return match.group(1)


def validate_kernel_macro_bridges(
    source: Path, adapter_path: Path, backend_runtime: dict[str, Any]
) -> list[dict[str, Any]]:
    """Validate state references hidden inside PostgreSQL header macros."""

    if (
        backend_runtime.get("schema_version") != 1
        or backend_runtime.get("kind") != "postgamma.backend-state-runtime"
        or not isinstance(backend_runtime.get("slots"), list)
    ):
        raise PgvectorContractError("backend-state runtime identity is invalid")
    runtime_slots = {
        slot.get("id"): slot
        for slot in backend_runtime["slots"]
        if isinstance(slot, dict) and isinstance(slot.get("id"), str)
    }
    adapter = adapter_path.read_text(encoding="utf-8")
    include = '#include "storage/lwlock.h"'
    include_offset = adapter.find(include)
    if include_offset < 0:
        raise PgvectorContractError(
            "pgvector kernel macro bridge does not include storage/lwlock.h"
        )

    results: list[dict[str, Any]] = []
    source_text = "\n".join(
        path.read_text(encoding="utf-8")
        for path in sorted((source / "src").glob("*.c"))
    )
    for bridge in KERNEL_MACRO_BRIDGES:
        source_macro = bridge["source_macro"]
        use_count = len(re.findall(rf"\b{re.escape(source_macro)}\b", source_text))
        if use_count != bridge["source_use_count"]:
            raise PgvectorContractError(
                f"pgvector kernel macro use changed: {source_macro}={use_count}"
            )
        definition = re.compile(
            rf"^\s*#\s*define\s+{re.escape(bridge['kernel_global'])}\s+"
            rf"{re.escape(bridge['accessor_macro'])}\s*$",
            re.MULTILINE,
        ).search(adapter)
        if definition is None or definition.start() <= include_offset:
            raise PgvectorContractError(
                "pgvector kernel macro bridge is missing or precedes its declaration: "
                + bridge["kernel_global"]
            )
        slot = runtime_slots.get(bridge["kernel_state_id"])
        if (
            not isinstance(slot, dict)
            or slot.get("owner") != "role"
            or slot.get("name") != bridge["kernel_global"]
            or not isinstance(slot.get("enum"), str)
        ):
            raise PgvectorContractError(
                "pgvector kernel macro bridge has no compatible runtime slot: "
                + bridge["kernel_state_id"]
            )
        results.append({**bridge, "observed_source_use_count": use_count})
    return results


def validate_contract(
    root: Path,
    upstream_manifest_path: Path,
    static_manifest_path: Path,
    state_policy_path: Path,
    state_report_path: Path,
    backend_runtime_path: Path,
    object_path: Path,
    nm: str,
) -> dict[str, Any]:
    upstream = read_json(upstream_manifest_path)
    required_upstream_fields = {
        "schema_version",
        "repository",
        "path",
        "ref_name",
        "version",
        "commit",
    }
    if upstream.get("schema_version") != 1 or set(upstream) != required_upstream_fields:
        raise PgvectorContractError("pgvector upstream manifest is invalid")
    source = (root / upstream["path"]).resolve()
    try:
        source.relative_to(root)
    except ValueError as exc:
        raise PgvectorContractError("pgvector source escapes the project root") from exc
    if git(source, "rev-parse", "HEAD") != upstream["commit"]:
        raise PgvectorContractError("pgvector submodule commit differs from its manifest")
    if git(source, "status", "--porcelain=v1", "--untracked-files=all"):
        raise PgvectorContractError("pgvector submodule is dirty")
    exact_tag = git(source, "describe", "--tags", "--exact-match", "HEAD")
    if exact_tag != upstream["ref_name"]:
        raise PgvectorContractError("pgvector tag differs from its manifest")

    control_path = source / "vector.control"
    sql_path = source / "sql/vector.sql"
    license_path = source / "LICENSE"
    adapter_path = root / "extensions/pgvector/include/postgamma_pgvector_adapter.h"
    for path in (
        control_path,
        sql_path,
        license_path,
        adapter_path,
        object_path,
        state_report_path,
        backend_runtime_path,
    ):
        if not path.is_file():
            raise PgvectorContractError(f"required pgvector input is missing: {path}")
    control = control_path.read_text(encoding="utf-8")
    sql = sql_path.read_text(encoding="utf-8")
    kernel_macro_bridges = validate_kernel_macro_bridges(
        source, adapter_path, read_json(backend_runtime_path)
    )
    if control_value(control, "default_version") != upstream["version"]:
        raise PgvectorContractError("pgvector control version differs from its manifest")
    if control_value(control, "module_pathname") != "$libdir/vector":
        raise PgvectorContractError("pgvector module pathname is not canonical")

    manifest = validate_manifest(read_json(static_manifest_path))
    modules = [module for module in manifest["modules"] if module["id"] == "pgvector"]
    if len(modules) != 1:
        raise PgvectorContractError("static module manifest has no unique pgvector module")
    module = modules[0]
    if (
        module["logical_name"] != "vector"
        or module["version"] != upstream["version"]
        or module["postgresql_major"] != 19
        or not module["sdk_contract"]
        or module["build"]
        != {
            "kind": "project-source",
            "source": "extensions/pgvector/src/pgvector_adapter.c",
        }
        or module["state_policy"]
        != "manifests/ownership/extensions/pgvector.json"
    ):
        raise PgvectorContractError("pgvector static module identity is invalid")
    declared_symbols = {
        entry["logical_name"]: entry["linker_name"] for entry in module["symbols"]
    }
    expected = expected_symbols(sql, source)
    if declared_symbols != expected:
        missing = sorted(set(expected) - set(declared_symbols))
        stale = sorted(set(declared_symbols) - set(expected))
        wrong = sorted(
            symbol
            for symbol in set(expected).intersection(declared_symbols)
            if expected[symbol] != declared_symbols[symbol]
        )
        raise PgvectorContractError(
            "pgvector SQL symbol closure changed: "
            f"missing={missing}, stale={stale}, wrong_linker_name={wrong}"
        )

    policy = parse_policy(read_json(state_policy_path))
    report = read_json(state_report_path)
    if (
        policy["extension_id"] != "pgvector"
        or policy["upstream"]
        != {
            "repository": upstream["repository"],
            "commit": upstream["commit"],
            "version": upstream["version"],
        }
        or report.get("schema_version") != 1
        or report.get("kind") != "postgamma.extension-state-alignment"
        or report.get("status") != "pass"
        or report.get("extension_id") != "pgvector"
        or report.get("upstream") != policy["upstream"]
    ):
        raise PgvectorContractError("pgvector state evidence has a stale identity")
    report_candidates = report.get("candidates")
    if not isinstance(report_candidates, list) or {
        item.get("id") for item in report_candidates if isinstance(item, dict)
    } != set(policy["decisions"]):
        raise PgvectorContractError("pgvector state evidence is not policy-exact")
    summary = report.get("summary")
    selection = report.get("selection")
    expected_state_summary = {
        "candidate_count": 16,
        "role_candidate_count": 16,
        "immutable_candidate_count": 0,
        "file_count": 18,
        "replacement_count": 97,
        "static_initializer_use_count": 0,
        "dormant_conditional_count": 0,
        "kernel_reference_count": 43,
        "kernel_reference_symbol_count": 7,
        "kernel_state_replacement_count": 32,
        "kernel_guc_replacement_count": 11,
        "kernel_immutable_reference_count": 0,
    }
    expected_kernel_references = [
        "external:CurrentMemoryContext",
        "external:debug_query_string",
        "external:maintenance_work_mem",
        "external:max_parallel_maintenance_workers",
        "external:pg_global_prng_state",
        "external:process_shared_preload_libraries_in_progress",
        "external:work_mem",
    ]
    if (
        not isinstance(summary, dict)
        or any(
            summary.get(name) != value
            for name, value in expected_state_summary.items()
        )
        or summary.get("candidate_count") != len(policy["decisions"])
        or report.get("kernel_references") != expected_kernel_references
        or not isinstance(selection, dict)
        or selection.get("domain") != "pgvector"
        or selection.get("translation_units") != 19
    ):
        raise PgvectorContractError("pgvector state transformation is incomplete")

    object_symbols = set(global_definitions(nm, object_path))
    missing_object_symbols = sorted(set(expected) - object_symbols)
    if missing_object_symbols:
        raise PgvectorContractError(
            "pgvector aggregate object misses SQL symbol(s): "
            + ", ".join(missing_object_symbols)
        )
    if "postgamma_pgvector_upstream_init" not in object_symbols:
        raise PgvectorContractError("pgvector upstream initializer was not renamed")

    return {
        "schema_version": 1,
        "kind": EVIDENCE_KIND,
        "status": "pass",
        "postgresql_major": 19,
        "pgvector": {
            "version": upstream["version"],
            "commit": upstream["commit"],
            "tag": upstream["ref_name"],
            "sql_function_count": len(sql_symbols(sql)),
            "parallel_worker_entry_count": len(parallel_worker_symbols(source)),
            "registered_symbol_count": len(expected),
            "object_global_symbol_count": len(object_symbols),
            "state_candidate_count": summary["candidate_count"],
            "state_replacement_count": summary["replacement_count"],
            "kernel_reference_count": summary["kernel_reference_count"],
            "kernel_reference_symbol_count": summary[
                "kernel_reference_symbol_count"
            ],
            "kernel_macro_bridge_count": len(kernel_macro_bridges),
        },
        "kernel_macro_bridges": kernel_macro_bridges,
        "inputs": {
            str(path.resolve()): sha256(path.resolve())
            for path in (
                upstream_manifest_path,
                static_manifest_path,
                state_policy_path,
                state_report_path,
                backend_runtime_path,
                control_path,
                sql_path,
                license_path,
                adapter_path,
                object_path,
            )
        },
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", required=True, type=Path)
    parser.add_argument("--upstream-manifest", required=True, type=Path)
    parser.add_argument("--static-manifest", required=True, type=Path)
    parser.add_argument("--state-policy", required=True, type=Path)
    parser.add_argument("--state-report", required=True, type=Path)
    parser.add_argument("--backend-state-runtime", required=True, type=Path)
    parser.add_argument("--object", required=True, type=Path)
    parser.add_argument("--nm", default="nm")
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    try:
        document = validate_contract(
            args.root.resolve(strict=True),
            args.upstream_manifest.resolve(strict=True),
            args.static_manifest.resolve(strict=True),
            args.state_policy.resolve(strict=True),
            args.state_report.resolve(strict=True),
            args.backend_state_runtime.resolve(strict=True),
            args.object.resolve(strict=True),
            args.nm,
        )
    except (KeyError, OSError, TypeError, ValueError) as exc:
        parser.error(str(exc))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(
        json.dumps(document, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    facts = document["pgvector"]
    print(
        "pgvector contract: pass "
        f"({facts['sql_function_count']} SQL functions, "
        f"{facts['parallel_worker_entry_count']} parallel worker entries, "
        f"{facts['registered_symbol_count']} registered symbols, "
        f"{facts['state_candidate_count']} role-state candidates, "
        f"{facts['kernel_macro_bridge_count']} kernel macro bridge)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
