#!/usr/bin/env python3
"""Build-identity-bound evidence for the bounded memory transport."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import platform
import subprocess
import sys
import tempfile
from collections import Counter
from pathlib import Path
from typing import Any

from check_bootstrap_evidence import baseline_identity


RUN_KIND = "postgamma.memory-transport-test"
EVIDENCE_KIND = "postgamma.memory-transport-evidence"
SCHEMA_VERSION = 1


class MemoryTransportCheckError(RuntimeError):
    """The transport implementation or its executable evidence is invalid."""


def _unique_object(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    counts = Counter(key for key, _value in pairs)
    duplicates = sorted(key for key, count in counts.items() if count > 1)
    if duplicates:
        raise MemoryTransportCheckError(
            "duplicate JSON key(s): " + ", ".join(duplicates)
        )
    return dict(pairs)


def load_json(path: Path) -> dict[str, Any]:
    try:
        document = json.loads(
            path.read_text(encoding="utf-8"), object_pairs_hook=_unique_object
        )
    except (OSError, json.JSONDecodeError, MemoryTransportCheckError) as exc:
        raise MemoryTransportCheckError(f"cannot read {path}: {exc}") from exc
    if not isinstance(document, dict):
        raise MemoryTransportCheckError(f"{path}: top-level value must be an object")
    return document


def exact_int(value: Any, label: str, minimum: int = 0) -> int:
    if not isinstance(value, int) or isinstance(value, bool) or value < minimum:
        raise MemoryTransportCheckError(f"{label} must be an integer >= {minimum}")
    return value


def resource_snapshot(value: Any, label: str) -> dict[str, int]:
    fields = {"active_transports", "endpoint_references", "allocated_bytes"}
    if not isinstance(value, dict) or set(value) != fields:
        raise MemoryTransportCheckError(f"{label} schema is invalid")
    return {field: exact_int(value[field], f"{label}.{field}") for field in fields}


def process_snapshot(value: Any, label: str) -> dict[str, int]:
    fields = {"descriptors", "threads"}
    if not isinstance(value, dict) or set(value) != fields:
        raise MemoryTransportCheckError(f"{label} schema is invalid")
    return {field: exact_int(value[field], f"{label}.{field}") for field in fields}


def validate_run_report(document: dict[str, Any]) -> dict[str, Any]:
    fields = {
        "schema_version",
        "kind",
        "status",
        "scenarios",
        "capacities",
        "repeated_lifecycles",
        "duplex_progress",
        "global_baseline",
        "global_final",
        "process_baseline",
        "process_final",
    }
    if set(document) != fields:
        raise MemoryTransportCheckError("memory transport report schema is invalid")
    if (
        document["schema_version"] != SCHEMA_VERSION
        or document["kind"] != RUN_KIND
        or document["status"] != "pass"
    ):
        raise MemoryTransportCheckError("memory transport report did not pass")
    exact_int(document["scenarios"], "scenarios", minimum=7)
    exact_int(document["repeated_lifecycles"], "repeated_lifecycles", minimum=100)
    if document["capacities"] != [1, 7, 64]:
        raise MemoryTransportCheckError("tiny queue capacities were not all tested")
    duplex = document["duplex_progress"]
    duplex_fields = {
        "capacity",
        "copy_bytes",
        "control_bytes",
        "both_queues_saturated",
        "wait_calls",
        "wait_wakeups",
        "timeouts",
    }
    if not isinstance(duplex, dict) or set(duplex) != duplex_fields:
        raise MemoryTransportCheckError("duplex progress report schema is invalid")
    capacity = exact_int(duplex["capacity"], "duplex_progress.capacity", minimum=1)
    copy_bytes = exact_int(
        duplex["copy_bytes"], "duplex_progress.copy_bytes", minimum=capacity + 1
    )
    control_bytes = exact_int(
        duplex["control_bytes"],
        "duplex_progress.control_bytes",
        minimum=capacity + 1,
    )
    wait_calls = exact_int(
        duplex["wait_calls"], "duplex_progress.wait_calls", minimum=1
    )
    wait_wakeups = exact_int(
        duplex["wait_wakeups"], "duplex_progress.wait_wakeups", minimum=1
    )
    if (
        capacity != 7
        or copy_bytes < 4096
        or control_bytes < 257
        or duplex["both_queues_saturated"] is not True
        or wait_calls == 0
        or wait_wakeups == 0
        or duplex["timeouts"] != 0
    ):
        raise MemoryTransportCheckError(
            "deterministic duplex saturation proof is incomplete"
        )
    global_baseline = resource_snapshot(document["global_baseline"], "global_baseline")
    global_final = resource_snapshot(document["global_final"], "global_final")
    if global_baseline != global_final or any(global_final.values()):
        raise MemoryTransportCheckError("transport resources did not return to zero")
    process_baseline = process_snapshot(
        document["process_baseline"], "process_baseline"
    )
    process_final = process_snapshot(document["process_final"], "process_final")
    if process_baseline != process_final or process_final["threads"] != 1:
        raise MemoryTransportCheckError("process resources did not return to baseline")
    return document


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def artifact(path: Path, build_root: Path) -> dict[str, str]:
    try:
        relative = path.resolve(strict=True).relative_to(build_root).as_posix()
    except (OSError, ValueError) as exc:
        raise MemoryTransportCheckError(
            f"transport artifact is missing or outside {build_root}: {path}"
        ) from exc
    return {"path": relative, "sha256": sha256(path)}


def write_text(path: Path, content: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content, encoding="utf-8")


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


def run_checked(
    label: str,
    command: list[str],
    stdout_path: Path,
    stderr_path: Path,
    environment: dict[str, str] | None = None,
) -> None:
    try:
        completed = subprocess.run(
            command,
            check=False,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            env=environment,
            timeout=15,
        )
    except (OSError, subprocess.TimeoutExpired) as exc:
        raise MemoryTransportCheckError(f"{label} could not run: {exc}") from exc
    write_text(stdout_path, completed.stdout)
    write_text(stderr_path, completed.stderr)
    if completed.returncode != 0:
        diagnostic = completed.stderr.strip() or completed.stdout.strip()
        raise MemoryTransportCheckError(
            f"{label} failed with status {completed.returncode}: {diagnostic}"
        )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--normal", required=True, type=Path)
    parser.add_argument("--asan", required=True, type=Path)
    parser.add_argument("--tsan", required=True, type=Path)
    parser.add_argument("--setarch", default="setarch")
    parser.add_argument("--run-report", required=True, type=Path)
    parser.add_argument("--normal-stdout", required=True, type=Path)
    parser.add_argument("--normal-stderr", required=True, type=Path)
    parser.add_argument("--asan-stdout", required=True, type=Path)
    parser.add_argument("--asan-stderr", required=True, type=Path)
    parser.add_argument("--tsan-stdout", required=True, type=Path)
    parser.add_argument("--tsan-stderr", required=True, type=Path)
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
        normal = args.normal.resolve(strict=True)
        asan = args.asan.resolve(strict=True)
        tsan = args.tsan.resolve(strict=True)
        run_report = args.run_report.resolve()
        normal_stdout = args.normal_stdout.resolve()
        normal_stderr = args.normal_stderr.resolve()
        asan_stdout = args.asan_stdout.resolve()
        asan_stderr = args.asan_stderr.resolve()
        tsan_stdout = args.tsan_stdout.resolve()
        tsan_stderr = args.tsan_stderr.resolve()
        for generated in (
            run_report,
            normal_stdout,
            normal_stderr,
            asan_stdout,
            asan_stderr,
            tsan_stdout,
            tsan_stderr,
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
        run_checked(
            "normal memory transport test",
            [str(normal), "--report", str(run_report)],
            normal_stdout,
            normal_stderr,
        )
        report = validate_run_report(load_json(run_report))
        sanitizer_environment = os.environ.copy()
        sanitizer_environment.update(
            {
                "ASAN_OPTIONS": "detect_leaks=1:halt_on_error=1",
                "UBSAN_OPTIONS": "halt_on_error=1:print_stacktrace=1",
            }
        )
        run_checked(
            "ASan/LSan/UBSan memory transport test",
            [str(asan)],
            asan_stdout,
            asan_stderr,
            sanitizer_environment,
        )
        thread_environment = os.environ.copy()
        thread_environment["TSAN_OPTIONS"] = "halt_on_error=1"
        run_checked(
            "TSan memory transport test",
            [args.setarch, platform.machine(), "-R", str(tsan)],
            tsan_stdout,
            tsan_stderr,
            thread_environment,
        )
        build_root = output.parent.parent.resolve()
        artifact_paths = [
            normal,
            asan,
            tsan,
            run_report,
            normal_stdout,
            normal_stderr,
            asan_stdout,
            asan_stderr,
            tsan_stdout,
            tsan_stderr,
        ]
        evidence = {
            "schema_version": SCHEMA_VERSION,
            "kind": EVIDENCE_KIND,
            "status": "pass",
            "baseline": baseline,
            "command": [str(Path(__file__).resolve()), *sys.argv[1:]],
            "artifacts": [artifact(path, build_root) for path in artifact_paths],
            "limitations": [
                "The resource topology check uses Linux /proc.",
                "This gate proves transport mechanics independently of PostgreSQL protocol semantics.",
            ],
            "metrics": {
                "capacities": report["capacities"],
                "repeated_lifecycles": report["repeated_lifecycles"],
                "copy_bytes": report["duplex_progress"]["copy_bytes"],
                "control_bytes": report["duplex_progress"]["control_bytes"],
                "both_queues_saturated": report["duplex_progress"][
                    "both_queues_saturated"
                ],
                "duplex_timeouts": report["duplex_progress"]["timeouts"],
                "asan_lsan_ubsan": "pass",
                "tsan": "pass",
            },
        }
        write_json(output, evidence)
    except (MemoryTransportCheckError, OSError, ValueError) as exc:
        parser.error(str(exc))
    print(
        "memory transport evidence: pass "
        f"({evidence['metrics']['copy_bytes']} COPY-like bytes, "
        "both queues saturated, ASan/LSan/UBSan/TSan pass)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
