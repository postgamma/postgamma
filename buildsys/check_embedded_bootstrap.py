#!/usr/bin/env python3
"""Run and record the embedded bootstrap repeated in-process bootstrap proof."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from collections import Counter
from pathlib import Path
from typing import Any

from check_bootstrap_evidence import LEAF_KIND, baseline_identity
from c_source_probe import CSourceProbeError, verify_source_seam
from postgresql_embedded_adapter import load_adapter as load_embedded_adapter
from reset_directory import is_strict_child


RUN_KIND = "postgamma.bootstrap-run"
TRACE_REPORT_KIND = "postgamma.bootstrap-syscalls"
FORBIDDEN_PROCESS_CALLS = frozenset(
    {"clone", "clone3", "fork", "vfork", "execveat"}
)
FORBIDDEN_SIGNAL_CALLS = frozenset(
    {"kill", "pidfd_send_signal", "rt_sigqueueinfo", "tgkill", "tkill"}
)


class BootstrapCheckError(RuntimeError):
    """The repeated in-process bootstrap proof failed or is stale."""


def _unique_object(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    counts = Counter(key for key, _value in pairs)
    duplicates = sorted(key for key, count in counts.items() if count > 1)
    if duplicates:
        raise BootstrapCheckError(
            "duplicate JSON key(s): " + ", ".join(duplicates)
        )
    return dict(pairs)


def load_json(path: Path) -> dict[str, Any]:
    try:
        document = json.loads(
            path.read_text(encoding="utf-8"), object_pairs_hook=_unique_object
        )
    except (OSError, json.JSONDecodeError, BootstrapCheckError) as exc:
        raise BootstrapCheckError(f"cannot read {path}: {exc}") from exc
    if not isinstance(document, dict):
        raise BootstrapCheckError(f"{path}: top-level value must be an object")
    return document


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def reset_child(path: Path, boundary: Path) -> None:
    path = path.resolve()
    boundary = boundary.resolve()
    if not is_strict_child(path, boundary):
        raise BootstrapCheckError(
            f"refusing to reset {path}: not a strict child of {boundary}"
        )
    if path.exists():
        shutil.rmtree(path)
    path.mkdir(parents=True)


def prepare_resource_root(
    postgres_source: Path, resource_root: Path, build_root: Path
) -> None:
    reset_child(resource_root, build_root)
    (resource_root / "bin").mkdir()
    source = postgres_source / "src/timezone/tznames"
    if not source.is_dir():
        raise BootstrapCheckError(f"PostgreSQL timezone source is missing: {source}")
    shutil.copytree(source, resource_root / "share/timezonesets")


def _exact_int(value: Any, label: str, minimum: int = 0) -> int:
    if not isinstance(value, int) or isinstance(value, bool) or value < minimum:
        raise BootstrapCheckError(f"{label} must be an integer >= {minimum}")
    return value


def validate_driver_report(document: dict[str, Any]) -> dict[str, Any]:
    expected = {
        "schema_version",
        "kind",
        "status",
        "library_loads",
        "bootstrap_calls",
        "distinct_system_identifiers",
        "baseline",
        "runs",
    }
    if set(document) != expected:
        raise BootstrapCheckError("bootstrap driver report schema is invalid")
    if document["schema_version"] != 1 or document["kind"] != RUN_KIND:
        raise BootstrapCheckError("bootstrap driver report identity is invalid")
    if (
        document["status"] != "pass"
        or document["library_loads"] != 1
        or document["bootstrap_calls"] != 2
        or document["distinct_system_identifiers"] is not True
    ):
        raise BootstrapCheckError("bootstrap driver did not complete the bootstrap proof")
    baseline = document["baseline"]
    baseline_fields = {
        "host_pid",
        "descriptors",
        "threads",
        "children",
        "sysv_mappings",
    }
    if not isinstance(baseline, dict) or set(baseline) != baseline_fields:
        raise BootstrapCheckError("bootstrap resource baseline is invalid")
    for name in baseline_fields:
        _exact_int(baseline[name], f"baseline.{name}")
    if baseline["threads"] != 1 or baseline["children"] != 0:
        raise BootstrapCheckError("bootstrap driver baseline is not single-threaded")

    runs = document["runs"]
    run_fields = {
        "generation",
        "status",
        "checks",
        "exit_code",
        "resources_remaining",
        "system_identifier",
        "cwd_restored",
        "stdin_restored",
        "host_pid_unchanged",
        "signals_restored",
        "pid_file_absent",
        "resources_restored",
        "driver_error",
    }
    if not isinstance(runs, list) or len(runs) != 2:
        raise BootstrapCheckError("bootstrap driver must report exactly two runs")
    identifiers: list[int] = []
    for index, run in enumerate(runs, start=1):
        if not isinstance(run, dict) or set(run) != run_fields:
            raise BootstrapCheckError(f"bootstrap run {index} schema is invalid")
        if (
            run["generation"] != index
            or run["status"] != 1
            or run["exit_code"] != 0
            or run["resources_remaining"] != 0
            or run["driver_error"] != 0
            or any(
                run[field] is not True
                for field in (
                    "cwd_restored",
                    "stdin_restored",
                    "host_pid_unchanged",
                    "signals_restored",
                    "pid_file_absent",
                    "resources_restored",
                )
            )
        ):
            raise BootstrapCheckError(f"bootstrap run {index} failed its contract")
        _exact_int(run["checks"], f"runs[{index - 1}].checks", minimum=5)
        identifiers.append(
            _exact_int(
                run["system_identifier"],
                f"runs[{index - 1}].system_identifier",
                minimum=1,
            )
        )
    if len(set(identifiers)) != 2:
        raise BootstrapCheckError("bootstrap system identifiers are not distinct")
    return document


def traced_syscalls(trace: str) -> list[str]:
    return re.findall(r"(?:^|\s)([a-zA-Z_][a-zA-Z0-9_]*)\(", trace, re.MULTILINE)


def audit_trace(trace: str, driver: Path) -> dict[str, Any]:
    calls = traced_syscalls(trace)
    counts = Counter(calls)
    forbidden_process = sorted(FORBIDDEN_PROCESS_CALLS.intersection(counts))
    forbidden_signals = sorted(FORBIDDEN_SIGNAL_CALLS.intersection(counts))
    if forbidden_process:
        raise BootstrapCheckError(
            "bootstrap invoked process-creation syscall(s): "
            + ", ".join(forbidden_process)
        )
    if forbidden_signals:
        raise BootstrapCheckError(
            "bootstrap invoked host-signal syscall(s): "
            + ", ".join(forbidden_signals)
        )
    exec_lines = [line for line in trace.splitlines() if "execve(" in line]
    if len(exec_lines) != 1 or str(driver) not in exec_lines[0]:
        raise BootstrapCheckError(
            "syscall trace must contain only the driver's initial execve"
        )
    attach_calls = counts.get("shmat", 0)
    detach_calls = counts.get("shmdt", 0)
    remove_calls = len(
        re.findall(r"(?:^|\s)shmctl\([^\n]*\bIPC_RMID\b", trace, re.MULTILINE)
    )
    if (attach_calls, detach_calls, remove_calls) != (2, 2, 2):
        raise BootstrapCheckError(
            "each bootstrap must attach, detach, and remove exactly one "
            "SysV shared-memory segment"
        )
    return {
        "schema_version": 1,
        "kind": TRACE_REPORT_KIND,
        "initial_execve_calls": 1,
        "process_creation_calls": 0,
        "host_signal_delivery_calls": 0,
        "sysv_attach_calls": attach_calls,
        "sysv_detach_calls": detach_calls,
        "sysv_remove_calls": remove_calls,
        "traced_syscall_count": len(calls),
    }


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


def artifact(path: Path, root: Path) -> dict[str, str]:
    try:
        relative = path.resolve().relative_to(root.resolve()).as_posix()
    except ValueError as exc:
        raise BootstrapCheckError(
            f"evidence artifact is outside the bootstrap build root: {path}"
        ) from exc
    if not path.is_file():
        raise BootstrapCheckError(f"evidence artifact is missing: {path}")
    return {"path": relative, "sha256": sha256(path)}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--library", required=True, type=Path)
    parser.add_argument("--driver", required=True, type=Path)
    parser.add_argument("--bootstrap-input", required=True, type=Path)
    parser.add_argument("--bootstrap-input-report", required=True, type=Path)
    parser.add_argument("--kernel-report", required=True, type=Path)
    parser.add_argument("--postgres-source", required=True, type=Path)
    parser.add_argument("--resource-root", required=True, type=Path)
    parser.add_argument("--run-root", required=True, type=Path)
    parser.add_argument("--run-report", required=True, type=Path)
    parser.add_argument("--trace", required=True, type=Path)
    parser.add_argument("--trace-report", required=True, type=Path)
    parser.add_argument("--seam-report", required=True, type=Path)
    parser.add_argument("--stderr", required=True, type=Path)
    parser.add_argument("--strace", default="strace")
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
        library = args.library.resolve(strict=True)
        driver = args.driver.resolve(strict=True)
        bootstrap_input = args.bootstrap_input.resolve(strict=True)
        input_report_path = args.bootstrap_input_report.resolve(strict=True)
        kernel_report_path = args.kernel_report.resolve(strict=True)
        postgres_source = args.postgres_source.resolve(strict=True)
        build_root = output.parent.parent
        resource_root = args.resource_root.resolve()
        run_root = args.run_root.resolve()
        run_report_path = args.run_report.resolve()
        trace_path = args.trace.resolve()
        trace_report_path = args.trace_report.resolve()
        seam_report_path = args.seam_report.resolve()
        stderr_path = args.stderr.resolve()
        for generated in (
            run_report_path,
            trace_path,
            trace_report_path,
            seam_report_path,
            stderr_path,
        ):
            generated.unlink(missing_ok=True)

        baseline, _profile = baseline_identity(
            args.upstream_manifest.resolve(),
            args.adapter.resolve(),
            args.embedded_adapter.resolve(),
            args.profile.resolve(),
            args.configure_state.resolve(),
            args.config_log.resolve(),
            args.build_profile,
        )
        input_report = load_json(input_report_path)
        if (
            input_report.get("kind") != "postgamma.bootstrap-input"
            or input_report.get("postgresql_major") != 19
            or input_report.get("output") != str(bootstrap_input)
            or input_report.get("output_sha256") != sha256(bootstrap_input)
        ):
            raise BootstrapCheckError("prepared bootstrap input report is stale")
        kernel_report = load_json(kernel_report_path)
        identity = kernel_report.get("identity")
        if not isinstance(identity, dict) or any(
            identity.get(field) != baseline[field]
            for field in (
                "upstream_commit",
                "configure_state_sha256",
                "toolchain_config_log_sha256",
                "embedded_adapter_id",
                "embedded_adapter_sha256",
            )
        ):
            raise BootstrapCheckError("kernel link report identity is stale")

        embedded_adapter = load_embedded_adapter(args.embedded_adapter.resolve())
        seam_facts = verify_source_seam(
            postgres_source, embedded_adapter["bootstrap_seam"]
        )
        seam_report = {
            "schema_version": 1,
            "kind": "postgamma.bootstrap-source-seam",
            "adapter_id": embedded_adapter["id"],
            **seam_facts,
        }
        write_json(seam_report_path, seam_report)

        prepare_resource_root(postgres_source, resource_root, build_root)
        reset_child(run_root, build_root)
        command = [
            args.strace,
            "-f",
            "-qq",
            "-o",
            str(trace_path),
            "-e",
            "trace=process,signal,ipc",
            str(driver),
            str(library),
            str(bootstrap_input),
            str(resource_root),
            str(run_root),
        ]
        try:
            completed = subprocess.run(
                command,
                check=False,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )
        except OSError as exc:
            raise BootstrapCheckError(f"cannot execute {args.strace}: {exc}") from exc
        run_report_path.parent.mkdir(parents=True, exist_ok=True)
        run_report_path.write_text(completed.stdout, encoding="utf-8")
        stderr_path.write_text(completed.stderr, encoding="utf-8")
        if completed.returncode != 0:
            diagnostic = completed.stderr.strip() or completed.stdout.strip()
            raise BootstrapCheckError(
                f"bootstrap driver failed with status {completed.returncode}: {diagnostic}"
            )
        driver_report = validate_driver_report(load_json(run_report_path))
        trace_report = audit_trace(trace_path.read_text(encoding="utf-8"), driver)
        write_json(trace_report_path, trace_report)

        control_files = [
            run_root / "cluster-a/global/pg_control",
            run_root / "cluster-b/global/pg_control",
        ]
        artifact_paths = [
            library,
            bootstrap_input,
            input_report_path,
            kernel_report_path,
            run_report_path,
            trace_path,
            trace_report_path,
            seam_report_path,
            stderr_path,
            *control_files,
        ]
        leaf = {
            "schema_version": 1,
            "kind": LEAF_KIND,
            "claim": "bootstrap_twice",
            "status": "pass",
            "baseline": baseline,
            "command": [str(Path(__file__).resolve()), *sys.argv[1:]],
            "artifacts": [artifact(path, build_root) for path in artifact_paths],
            "limitations": [
                "The driver restores stdin, cwd, signal dispositions, and the signal mask outside the library; product code may not rely on that test boundary.",
                "The proof uses Linux /proc resource counters and strace syscall evidence.",
                "The bootstrap bridge supplies a synthetic resource-root executable path and is not the product API.",
                "The bootstrap bridge uses /proc/self/exe as a temporary bootstrap argv[0].",
                "The proof covers PostgreSQL BootstrapModeMain, not complete initdb post-bootstrap SQL.",
            ],
            "metrics": {
                "library_loads": driver_report["library_loads"],
                "host_pid": driver_report["baseline"]["host_pid"],
                "bootstrap_calls": driver_report["bootstrap_calls"],
                "distinct_system_identifiers": 2,
                "descriptors_at_baseline": driver_report["baseline"]["descriptors"],
                "threads_at_baseline": driver_report["baseline"]["threads"],
                "children_at_baseline": driver_report["baseline"]["children"],
                "sysv_mappings_at_baseline": driver_report["baseline"]["sysv_mappings"],
                "process_creation_calls": trace_report["process_creation_calls"],
                "host_signal_delivery_calls": trace_report[
                    "host_signal_delivery_calls"
                ],
                "sysv_attach_calls": trace_report["sysv_attach_calls"],
                "sysv_detach_calls": trace_report["sysv_detach_calls"],
                "sysv_remove_calls": trace_report["sysv_remove_calls"],
                "bootstrap_source_anchors": sum(
                    seam_report["callee_matches"].values()
                ),
            },
        }
        write_json(output, leaf)
    except (
        BootstrapCheckError,
        CSourceProbeError,
        OSError,
        ValueError,
        json.JSONDecodeError,
    ) as exc:
        parser.error(str(exc))
    print(
        "embedded bootstrap evidence: pass "
        "(2 calls, 2 system identifiers, 0 process creation calls)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
