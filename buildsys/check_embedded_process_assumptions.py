#!/usr/bin/env python3
"""Freeze and verify the embedded bootstrap process-model integration surface."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import sys
import tempfile
from collections import Counter
from pathlib import Path
from typing import Any

from check_bootstrap_evidence import baseline_identity
from c_source_probe import CSourceProbeError, function_span, sanitize_c
from postgresql_embedded_adapter import load_adapter as load_embedded_adapter


REPORT_KIND = "postgamma.embedded-process-assumptions"
SIGNAL_CALLS = frozenset(
    {"abort", "kill", "pthread_kill", "raise", "sigqueue", "tgkill", "tkill"}
)
PROCESS_CALLS = frozenset({"fork", "popen", "posix_spawn", "system", "vfork"})
REQUIRED_CATEGORIES = frozenset(
    {
        "crash_restart",
        "cwd",
        "dynamic_loader",
        "exit",
        "extension_shmem",
        "frontend_tool",
        "libpq_network",
        "pidfile",
        "process_creation",
        "server_network",
        "signal_emission",
        "thread_stack",
    }
)


class ProcessAssumptionError(RuntimeError):
    """A process-model source anchor or executable bootstrap fact is incomplete."""


def load_json(path: Path) -> dict[str, Any]:
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise ProcessAssumptionError(f"cannot read {path}: {exc}") from exc
    if not isinstance(document, dict):
        raise ProcessAssumptionError(f"{path}: top-level value must be an object")
    return document


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def line_column(source: str, offset: int) -> tuple[int, int]:
    line_start = source.rfind("\n", 0, offset) + 1
    return source.count("\n", 0, offset) + 1, offset - line_start + 1


def assumption_facts(
    assumption: dict[str, Any], project_root: Path, postgres_source: Path
) -> tuple[dict[str, Any], set[tuple[str, str, int]]]:
    root = project_root if assumption["source_root"] == "project" else postgres_source
    path = (root / assumption["source_file"]).resolve()
    try:
        path.relative_to(root.resolve())
        raw = path.read_text(encoding="utf-8")
    except (OSError, ValueError) as exc:
        raise ProcessAssumptionError(
            f"cannot read assumption source {path}: {exc}"
        ) from exc
    sanitized = sanitize_c(raw)
    scope_start = 0
    scope_end = len(raw)
    if assumption["scope"] != "__file__":
        try:
            scope_start, scope_end = function_span(
                sanitized, assumption["scope"], 1
            )
        except CSourceProbeError as exc:
            raise ProcessAssumptionError(
                f"assumption {assumption['id']} scope drifted: {exc}"
            ) from exc
    if assumption["probe_kind"] == "call":
        searched = sanitized[scope_start:scope_end]
        pattern = re.compile(rf"\b{re.escape(assumption['probe'])}\s*\(")
    else:
        searched = raw[scope_start:scope_end]
        pattern = re.compile(re.escape(assumption["probe"]))
    offsets = [scope_start + match.start() for match in pattern.finditer(searched)]
    if len(offsets) != assumption["expected_matches"]:
        locations = ", ".join(
            f"{assumption['source_file']}:{line_column(raw, offset)[0]}"
            for offset in offsets
        ) or "none"
        raise ProcessAssumptionError(
            f"assumption {assumption['id']} expected "
            f"{assumption['expected_matches']} match(es), found {len(offsets)} "
            f"at {locations}"
        )
    matches = []
    inventory_keys: set[tuple[str, str, int]] = set()
    for offset in offsets:
        line, column = line_column(raw, offset)
        matches.append({"line": line, "column": column})
        if (
            assumption["source_root"] == "project"
            and assumption["probe_kind"] == "call"
            and assumption["probe"] in SIGNAL_CALLS | PROCESS_CALLS
        ):
            inventory_keys.add(
                (assumption["source_file"], assumption["probe"], offset)
            )
    return (
        {
            **assumption,
            "actual_matches": len(offsets),
            "matches": matches,
            "source_sha256": sha256(path),
            "status": "classified",
        },
        inventory_keys,
    )


def project_process_calls(project_root: Path) -> set[tuple[str, str, int]]:
    calls: set[tuple[str, str, int]] = set()
    pattern = re.compile(
        rb"\b(" + b"|".join(
            re.escape(name.encode("ascii"))
            for name in sorted(SIGNAL_CALLS | PROCESS_CALLS)
        ) + rb")\s*\("
    )
    for relative_root in (Path("runtime/src"), Path("embedded-c/src")):
        root = project_root / relative_root
        for path in sorted(root.glob("*.c")):
            sanitized = sanitize_c(path.read_text(encoding="utf-8"))
            for match in pattern.finditer(sanitized.encode("utf-8")):
                calls.add(
                    (
                        path.relative_to(project_root).as_posix(),
                        match.group(1).decode("ascii"),
                        match.start(),
                    )
                )
    return calls


def require_leaf(
    path: Path, baseline: dict[str, Any], claim: str
) -> dict[str, Any]:
    document = load_json(path)
    if (
        document.get("kind") != "postgamma.bootstrap-leaf-evidence"
        or document.get("claim") != claim
        or document.get("status") != "pass"
        or document.get("baseline") != baseline
        or not isinstance(document.get("metrics"), dict)
        or not isinstance(document.get("limitations"), list)
    ):
        raise ProcessAssumptionError(f"bootstrap leaf evidence is stale: {path}")
    return document


def require_zero(metrics: dict[str, Any], fields: tuple[str, ...], label: str) -> None:
    bad = [field for field in fields if metrics.get(field) != 0]
    if bad:
        raise ProcessAssumptionError(
            f"{label} has nonzero forbidden metric(s): " + ", ".join(bad)
        )


def validate_runtime_evidence(
    baseline: dict[str, Any],
    static_path: Path,
    bootstrap_path: Path,
    private_path: Path,
    bootstrap_run_path: Path,
) -> tuple[dict[str, Any], list[str]]:
    static = require_leaf(static_path, baseline, "hidden_static_extension")
    bootstrap = require_leaf(bootstrap_path, baseline, "bootstrap_twice")
    private = require_leaf(
        private_path, baseline, "private_libpq_memory_transport"
    )
    static_metrics = static["metrics"]
    bootstrap_metrics = bootstrap["metrics"]
    private_metrics = private["metrics"]
    require_zero(
        static_metrics,
        (
            "dynamic_load_attempts",
            "direct_dynamic_loader_calls",
            "network_syscalls",
            "process_creation_syscalls",
            "host_signal_delivery_syscalls",
        ),
        "static extension proof",
    )
    require_zero(
        bootstrap_metrics,
        ("process_creation_calls", "host_signal_delivery_calls"),
        "bootstrap proof",
    )
    require_zero(
        private_metrics,
        (
            "network_syscalls",
            "process_creation_syscalls",
            "host_signal_delivery_syscalls",
        ),
        "private libpq proof",
    )
    for label, metrics in (
        ("static extension proof", static_metrics),
        ("private libpq proof", private_metrics),
    ):
        if (
            metrics.get("host_pid_unchanged") is not True
            or metrics.get("resources_restored") is not True
            or metrics.get("baseline_threads") != 1
            or metrics.get("baseline_children") != 0
        ):
            raise ProcessAssumptionError(
                f"{label} did not restore its host resource baseline"
            )
    bootstrap_run = load_json(bootstrap_run_path)
    if bootstrap_run.get("status") != "pass":
        raise ProcessAssumptionError("bootstrap run report is not a pass")
    runs = bootstrap_run.get("runs")
    if not isinstance(runs, list) or len(runs) != 2 or any(
        not isinstance(run, dict)
        or run.get("host_pid_unchanged") is not True
        or run.get("resources_restored") is not True
        for run in runs
    ):
        raise ProcessAssumptionError(
            "bootstrap invocations did not restore their host resource baseline"
        )
    limitations = sorted(
        set(static["limitations"] + bootstrap["limitations"] + private["limitations"])
    )
    if not any("test" in limitation.lower() for limitation in limitations):
        raise ProcessAssumptionError(
            "bootstrap evidence omitted its test-only compatibility declarations"
        )
    return (
        {
            "static_extension": {
                "path": static_path.name,
                "sha256": sha256(static_path),
                "host_pid": static_metrics["host_pid"],
                "resources_restored": True,
                "process_creation_syscalls": 0,
                "network_syscalls": 0,
                "host_signal_delivery_syscalls": 0,
                "unknown_module_loads": 0,
            },
            "bootstrap_twice": {
                "path": bootstrap_path.name,
                "sha256": sha256(bootstrap_path),
                "host_pid": bootstrap_metrics["host_pid"],
                "resources_restored": True,
                "process_creation_syscalls": 0,
                "host_signal_delivery_syscalls": 0,
            },
            "private_libpq": {
                "path": private_path.name,
                "sha256": sha256(private_path),
                "host_pid": private_metrics["host_pid"],
                "resources_restored": True,
                "process_creation_syscalls": 0,
                "network_syscalls": 0,
                "host_signal_delivery_syscalls": 0,
            },
        },
        limitations,
    )


def write_json(path: Path, document: dict[str, Any]) -> None:
    content = json.dumps(document, indent=2, sort_keys=True) + "\n"
    path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(
        mode="w",
        encoding="utf-8",
        dir=path.parent,
        prefix=f".{path.name}.",
        delete=False,
    ) as handle:
        handle.write(content)
        temporary = Path(handle.name)
    try:
        os.replace(temporary, path)
    finally:
        temporary.unlink(missing_ok=True)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--project-root", required=True, type=Path)
    parser.add_argument("--postgres-source", required=True, type=Path)
    parser.add_argument("--static-evidence", required=True, type=Path)
    parser.add_argument("--bootstrap-evidence", required=True, type=Path)
    parser.add_argument("--private-libpq-evidence", required=True, type=Path)
    parser.add_argument("--bootstrap-run-report", required=True, type=Path)
    parser.add_argument("--profile", required=True, type=Path)
    parser.add_argument("--upstream-manifest", required=True, type=Path)
    parser.add_argument("--adapter", required=True, type=Path)
    parser.add_argument("--embedded-adapter", required=True, type=Path)
    parser.add_argument("--configure-state", required=True, type=Path)
    parser.add_argument("--config-log", required=True, type=Path)
    parser.add_argument("--build-profile", required=True)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    output = args.output.resolve()
    output.unlink(missing_ok=True)
    try:
        project_root = args.project_root.resolve(strict=True)
        postgres_source = args.postgres_source.resolve(strict=True)
        baseline, _profile = baseline_identity(
            args.upstream_manifest.resolve(),
            args.adapter.resolve(),
            args.embedded_adapter.resolve(),
            args.profile.resolve(),
            args.configure_state.resolve(),
            args.config_log.resolve(),
            args.build_profile,
        )
        adapter = load_embedded_adapter(args.embedded_adapter.resolve())
        records: list[dict[str, Any]] = []
        classified_project_calls: set[tuple[str, str, int]] = set()
        for assumption in adapter["process_assumptions"]:
            facts, inventory_keys = assumption_facts(
                assumption, project_root, postgres_source
            )
            records.append(facts)
            classified_project_calls.update(inventory_keys)
        actual_project_calls = project_process_calls(project_root)
        if actual_project_calls != classified_project_calls:
            missing = sorted(actual_project_calls - classified_project_calls)
            stale = sorted(classified_project_calls - actual_project_calls)
            raise ProcessAssumptionError(
                "project process-call inventory is incomplete; missing="
                f"{missing}, stale={stale}"
            )
        categories = Counter(record["category"] for record in records)
        if set(categories) != REQUIRED_CATEGORIES:
            raise ProcessAssumptionError(
                "process-assumption categories are incomplete: "
                + ", ".join(sorted(REQUIRED_CATEGORIES - set(categories)))
            )
        classifications = Counter(record["classification"] for record in records)
        runtime, limitations = validate_runtime_evidence(
            baseline,
            args.static_evidence.resolve(strict=True),
            args.bootstrap_evidence.resolve(strict=True),
            args.private_libpq_evidence.resolve(strict=True),
            args.bootstrap_run_report.resolve(strict=True),
        )
        report = {
            "schema_version": 1,
            "kind": REPORT_KIND,
            "status": "pass",
            "baseline": baseline,
            "product_ready": False,
            "embedded_kernel_surface_frozen": True,
            "record_count": len(records),
            "category_counts": dict(sorted(categories.items())),
            "classification_counts": dict(sorted(classifications.items())),
            "project_process_call_count": len(actual_project_calls),
            "records": records,
            "runtime_evidence": runtime,
            "test_only_compatibility_declarations": limitations,
            "command": [str(Path(__file__).resolve()), *sys.argv[1:]],
        }
        write_json(output, report)
    except (OSError, ValueError, ProcessAssumptionError) as exc:
        parser.error(str(exc))
    print(
        "embedded process assumptions: pass "
        f"({len(records)} classified records, "
        f"{len(actual_project_calls)} project process calls)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
