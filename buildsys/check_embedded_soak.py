#!/usr/bin/env python3
"""Prove repeated PG19 query and startup-fault lifecycles in one process."""

from __future__ import annotations

import argparse
import re
import tempfile
from pathlib import Path
from typing import Sequence

from check_embedded_lifecycle import (
    LifecycleCheckError,
    input_identity,
    installed_prefix,
    run_checked,
    runtime_environment,
    sha256,
    write_json,
)


MARKER = "POSTGAMMA_KERNEL_SOAK"
MINIMUM_ITERATIONS = 1000
MAXIMUM_ITERATIONS = 1_000_000
FAULT_POINTS = 10
FAULT_COVERAGE = (1 << FAULT_POINTS) - 1
MAPPING_DELTA_BUDGET = 32
VIRTUAL_KIB_DELTA_BUDGET = 262_144
RESIDENT_KIB_DELTA_BUDGET = 131_072
MAPPING_STEADY_DRIFT_BUDGET = 4
VIRTUAL_KIB_STEADY_DRIFT_BUDGET = 32_768
RESIDENT_KIB_STEADY_DRIFT_BUDGET = 16_384
ALLOCATOR_LIVE_DELTA_BUDGET = 8_388_608
ALLOCATOR_LIVE_STEADY_DRIFT_BUDGET = 1_048_576


class SoakCheckError(RuntimeError):
    """The same-process embedded soak contract was not met."""


def validate_iterations(iterations: int) -> int:
    if iterations < MINIMUM_ITERATIONS or iterations > MAXIMUM_ITERATIONS:
        raise SoakCheckError(
            f"iterations must be between {MINIMUM_ITERATIONS} "
            f"and {MAXIMUM_ITERATIONS}"
        )
    return iterations


def expected_schedule(iterations: int) -> dict[str, int]:
    validate_iterations(iterations)
    data_b = iterations // 3
    immediate = (iterations + 1) // 3
    return {
        "data_a_cycles": iterations - data_b,
        "data_b_cycles": data_b,
        "fast_shutdowns": iterations - immediate,
        "immediate_shutdowns": immediate,
        "recovery_queries": (iterations + 1) // 3,
    }


def parse_integer(values: dict[str, str], name: str) -> int:
    try:
        return int(values.get(name, ""), 0)
    except ValueError as exc:
        raise SoakCheckError(f"soak marker {name} is not an integer") from exc


def parse_marker(output: str, iterations: int, seed: int) -> dict[str, int | str]:
    lines = [line for line in output.splitlines() if MARKER in line]
    if len(lines) != 1:
        raise SoakCheckError(
            f"expected one {MARKER} line, found {len(lines)}"
        )
    raw = dict(re.findall(r"([a-z_]+)=([^\s]+)", lines[0]))
    schedule = expected_schedule(iterations)
    expected_numeric = {
        "seed": seed,
        "iterations": iterations,
        "lifecycle_cycles": iterations,
        "query_cycles": iterations,
        "fault_cycles": iterations,
        **schedule,
        "fault_points": FAULT_POINTS,
        "fault_coverage": FAULT_COVERAGE,
        "fault_injections": iterations,
        "failure_records": iterations,
        "cleanup_failures": 0,
        "network_calls": 0,
        "process_checks": iterations * 2,
        "mapping_checks": iterations * 2,
        "memory_checks": iterations * 2,
        "active_transports": 0,
        "endpoint_references": 0,
        "active_locks": 0,
        "active_instances": 0,
        "active_memory_contexts": 0,
        "mapping_delta_budget": MAPPING_DELTA_BUDGET,
        "virtual_kib_delta_budget": VIRTUAL_KIB_DELTA_BUDGET,
        "resident_kib_delta_budget": RESIDENT_KIB_DELTA_BUDGET,
        "mapping_steady_drift_budget": MAPPING_STEADY_DRIFT_BUDGET,
        "virtual_kib_steady_drift_budget": VIRTUAL_KIB_STEADY_DRIFT_BUDGET,
        "resident_kib_steady_drift_budget": RESIDENT_KIB_STEADY_DRIFT_BUDGET,
        "allocator_live_delta_budget": ALLOCATOR_LIVE_DELTA_BUDGET,
        "allocator_live_steady_drift_budget": (
            ALLOCATOR_LIVE_STEADY_DRIFT_BUDGET
        ),
    }
    parsed: dict[str, int | str] = dict(raw)
    for name, expected in expected_numeric.items():
        actual = parse_integer(raw, name)
        if actual != expected:
            raise SoakCheckError(
                f"soak marker {name} is {actual}, expected {expected}"
            )
        parsed[name] = actual
    for name in (
        "fault_callbacks",
        "secure_reads",
        "secure_writes",
        "duration_ms",
        "host_pid",
        "mapping_baseline",
        "mapping_peak",
        "mapping_final",
        "virtual_kib_baseline",
        "virtual_kib_peak",
        "virtual_kib_final",
        "resident_kib_baseline",
        "resident_kib_peak",
        "resident_kib_final",
        "mapping_first_half_peak",
        "mapping_second_half_peak",
        "virtual_kib_first_half_peak",
        "virtual_kib_second_half_peak",
        "resident_kib_first_half_peak",
        "resident_kib_second_half_peak",
        "allocator_live_baseline",
        "allocator_live_peak",
        "allocator_live_final",
        "allocator_live_first_half_peak",
        "allocator_live_second_half_peak",
    ):
        parsed[name] = parse_integer(raw, name)
    if not iterations <= int(parsed["fault_callbacks"]) <= iterations * 10:
        raise SoakCheckError("soak fault callback count is outside bounds")
    if int(parsed["secure_reads"]) < iterations:
        raise SoakCheckError("soak secure read count is below query count")
    if int(parsed["secure_writes"]) < iterations:
        raise SoakCheckError("soak secure write count is below query count")
    if int(parsed["duration_ms"]) <= 0 or int(parsed["host_pid"]) <= 0:
        raise SoakCheckError("soak duration or host PID is not positive")
    for prefix, budget in (
        ("mapping", MAPPING_DELTA_BUDGET),
        ("virtual_kib", VIRTUAL_KIB_DELTA_BUDGET),
        ("resident_kib", RESIDENT_KIB_DELTA_BUDGET),
    ):
        baseline = int(parsed[f"{prefix}_baseline"])
        peak = int(parsed[f"{prefix}_peak"])
        final = int(parsed[f"{prefix}_final"])
        if baseline <= 0 or peak < baseline or final <= 0 or final > peak:
            raise SoakCheckError(f"soak {prefix} high-water values are invalid")
        if peak - baseline > budget or final - baseline > budget:
            raise SoakCheckError(f"soak {prefix} exceeded its closed-state budget")
    for prefix, budget in (
        ("mapping", MAPPING_STEADY_DRIFT_BUDGET),
        ("virtual_kib", VIRTUAL_KIB_STEADY_DRIFT_BUDGET),
        ("resident_kib", RESIDENT_KIB_STEADY_DRIFT_BUDGET),
    ):
        baseline = int(parsed[f"{prefix}_baseline"])
        peak = int(parsed[f"{prefix}_peak"])
        first = int(parsed[f"{prefix}_first_half_peak"])
        second = int(parsed[f"{prefix}_second_half_peak"])
        if first < baseline or second < baseline or peak != max(first, second):
            raise SoakCheckError(f"soak {prefix} phase peaks are invalid")
        if second > first and second - first > budget:
            raise SoakCheckError(f"soak {prefix} steady-state drift is too large")
    allocator_supported = raw.get("allocator_metrics_supported") == "true"
    if raw.get("allocator_metrics_supported") not in ("true", "false"):
        raise SoakCheckError("soak allocator_metrics_supported is invalid")
    allocator_values = {
        name: int(parsed[name])
        for name in (
            "allocator_live_baseline",
            "allocator_live_peak",
            "allocator_live_final",
            "allocator_live_first_half_peak",
            "allocator_live_second_half_peak",
        )
    }
    if allocator_supported:
        baseline = allocator_values["allocator_live_baseline"]
        peak = allocator_values["allocator_live_peak"]
        final = allocator_values["allocator_live_final"]
        first = allocator_values["allocator_live_first_half_peak"]
        second = allocator_values["allocator_live_second_half_peak"]
        if baseline <= 0 or peak < baseline or final <= 0 or final > peak:
            raise SoakCheckError("soak allocator live high-water values are invalid")
        if peak - baseline > ALLOCATOR_LIVE_DELTA_BUDGET:
            raise SoakCheckError("soak allocator live usage exceeded its budget")
        if first < baseline or second < baseline or peak != max(first, second):
            raise SoakCheckError("soak allocator live phase peaks are invalid")
        if second > first and second - first > ALLOCATOR_LIVE_STEADY_DRIFT_BUDGET:
            raise SoakCheckError("soak allocator live steady-state drift is too large")
    elif any(allocator_values.values()):
        raise SoakCheckError("unsupported allocator metrics must be zero")
    for name in (
        "resources_restored",
        "mappings_bounded",
        "allocator_live_bounded",
        "steady_state_bounded",
        "process_globals_restored",
        "pid_files_absent",
    ):
        if raw.get(name) != "true":
            raise SoakCheckError(f"soak marker {name} is not true")
    return parsed


def validate_recovery_logs(
    error_output: str, iterations: int
) -> dict[str, int | str]:
    schedule = expected_schedule(iterations)
    expected_recoveries = schedule["recovery_queries"] + 1
    expected_immediate = schedule["immediate_shutdowns"] + 1
    recoveries = error_output.count("automatic recovery in progress")
    interrupted = error_output.count("database system was interrupted")
    immediate = error_output.count("received immediate shutdown request")
    ready = error_output.count("database system is ready to accept connections")
    minimum_ready = iterations + 3
    if recoveries < expected_recoveries or interrupted < expected_recoveries:
        raise SoakCheckError(
            "soak did not expose every scheduled recovery in PostgreSQL logs"
        )
    if immediate < expected_immediate:
        raise SoakCheckError(
            "soak did not expose every immediate shutdown in PostgreSQL logs"
        )
    if ready < minimum_ready:
        raise SoakCheckError("soak did not reach READY for every query lifecycle")
    return {
        "status": "pass",
        "automatic_recovery_messages": recoveries,
        "interrupted_messages": interrupted,
        "immediate_shutdown_messages": immediate,
        "ready_messages": ready,
        "minimum_expected_recoveries": expected_recoveries,
        "minimum_expected_ready": minimum_ready,
    }


def initialize_cluster(
    initdb: Path,
    data_directory: Path,
    environment: dict[str, str],
) -> None:
    run_checked(
        [
            str(initdb),
            "-D",
            str(data_directory),
            "--username=postgamma",
            "--auth=trust",
            "--encoding=UTF8",
            "--locale=C",
            "--no-sync",
            "--no-instructions",
        ],
        environment=environment,
        timeout=60.0,
    )
    config = data_directory / "postgresql.conf"
    config.write_text(
        config.read_text(encoding="utf-8")
        + "\n# PostGamma cluster lifecycle same-process soak profile.\n"
        + "shared_buffers = '24MB'\n"
        + "max_connections = 6\n"
        + "restart_after_crash = off\n",
        encoding="utf-8",
    )


def write_artifacts(
    stdout_path: Path,
    stderr_path: Path,
    stdout: str,
    stderr: str,
) -> None:
    stdout_path.parent.mkdir(parents=True, exist_ok=True)
    stdout_path.write_text(stdout, encoding="utf-8")
    stderr_path.write_text(stderr, encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--make", default="make")
    parser.add_argument("--jobs", type=int, default=1)
    parser.add_argument("--postgres-build", required=True, type=Path)
    parser.add_argument("--driver", required=True, type=Path)
    parser.add_argument("--work-root", required=True, type=Path)
    parser.add_argument("--iterations", required=True, type=int)
    parser.add_argument("--seed", required=True, type=int)
    parser.add_argument("--stdout", required=True, type=Path)
    parser.add_argument("--stderr", required=True, type=Path)
    parser.add_argument("--run-report", required=True, type=Path)
    parser.add_argument("--lifecycle-evidence", required=True, type=Path)
    parser.add_argument("--query-evidence", required=True, type=Path)
    parser.add_argument("--input", action="append", default=[], type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()

    generated_paths = (args.stdout, args.stderr, args.run_report, args.output)
    for path in generated_paths:
        resolved = path.resolve()
        resolved.parent.mkdir(parents=True, exist_ok=True)
        resolved.unlink(missing_ok=True)
    try:
        iterations = validate_iterations(args.iterations)
        if args.seed <= 0 or args.seed > (1 << 64) - 1:
            raise SoakCheckError("seed must be a nonzero uint64")
        postgres_build = args.postgres_build.resolve(strict=True)
        driver = args.driver.resolve(strict=True)
        inputs = [path.resolve(strict=True) for path in args.input]
        lifecycle_evidence = args.lifecycle_evidence.resolve(strict=True)
        query_evidence = args.query_evidence.resolve(strict=True)
        work_root = args.work_root.resolve()
        work_root.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(
            prefix="soak-", dir=work_root
        ) as temporary:
            temporary_path = Path(temporary)
            install_root = temporary_path / "install"
            run_checked(
                [
                    args.make,
                    "-C",
                    str(postgres_build),
                    f"-j{args.jobs}",
                    "install",
                    f"DESTDIR={install_root}",
                ],
                timeout=300.0,
            )
            prefix = installed_prefix(install_root)
            environment = runtime_environment(prefix)
            environment["LC_ALL"] = "C"
            data_a = temporary_path / "data-a"
            data_b = temporary_path / "data-b"
            initialize_cluster(prefix / "bin" / "initdb", data_a, environment)
            initialize_cluster(prefix / "bin" / "initdb", data_b, environment)
            command = [
                str(driver),
                str(data_a),
                str(data_b),
                str(prefix / "bin" / "postgres"),
                str(prefix),
                str(iterations),
                str(args.seed),
            ]
            completed = run_checked(
                command,
                environment=environment,
                timeout=max(1800.0, iterations * 2.0),
                check=False,
            )
            write_artifacts(
                args.stdout, args.stderr, completed.stdout, completed.stderr
            )
            if completed.returncode != 0:
                raise SoakCheckError(
                    f"command failed ({completed.returncode}): "
                    f"{' '.join(command)}\n"
                    f"stdout:\n{completed.stdout}\n"
                    f"stderr tail:\n{completed.stderr[-16000:]}"
                )
            marker = parse_marker(completed.stdout, iterations, args.seed)
            recovery = validate_recovery_logs(completed.stderr, iterations)
            if (data_a / "postmaster.pid").exists() or (
                data_b / "postmaster.pid"
            ).exists():
                raise SoakCheckError("soak left postmaster.pid")

        run_report = {
            "schema_version": 1,
            "kind": "postgamma.lifecycle-soak-run",
            "status": "pass",
            "seed": args.seed,
            "iterations": iterations,
            "replay": {
                "make_target": "lifecycle-soak-check",
                "make_variables": {
                    "EMBEDDED_KERNEL_SOAK_ITERATIONS": iterations,
                    "EMBEDDED_KERNEL_SOAK_SEED": args.seed,
                },
            },
            "marker": marker,
            "recovery": recovery,
        }
        write_json(args.run_report, run_report)
        report = {
            "schema_version": 1,
            "kind": "postgamma.lifecycle-soak-evidence",
            "status": "pass",
            "claim": "same_process_minimum_1000_query_and_fault_lifecycles",
            "postgresql_major": 19,
            "driver_sha256": sha256(driver),
            "inputs": input_identity(inputs),
            "run": run_report,
            "composed_evidence": {
                "host_syscall_contract": str(lifecycle_evidence),
                "memory_query_contract": str(query_evidence),
            },
            "artifacts": {
                "stdout": str(args.stdout.resolve()),
                "stderr": str(args.stderr.resolve()),
                "run_report": str(args.run_report.resolve()),
            },
        }
        write_json(args.output, report)
    except (LifecycleCheckError, SoakCheckError, OSError, ValueError) as exc:
        parser.error(str(exc))
    print(
        "embedded soak evidence: pass "
        f"({args.iterations} query lifecycles plus "
        f"{args.iterations} fault lifecycles in one process)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
