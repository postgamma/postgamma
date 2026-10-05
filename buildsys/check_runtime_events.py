#!/usr/bin/env python3
"""Prove diagnostics, settings, and routed event semantics."""

from __future__ import annotations

import argparse
import json
import os
import re
import resource
import tempfile
from contextlib import contextmanager
from pathlib import Path
from typing import Iterator

from c_source_probe import CSourceProbeError, function_span, sanitize_c
from check_embedded_lifecycle import (
    LifecycleCheckError,
    input_identity,
    installed_prefix,
    run_checked,
    runtime_environment,
    write_json,
)
from check_embedded_public_api import PublicApiCheckError, audit_trace


MARKER = "POSTGAMMA_KERNEL_3_EVENTS"
EVIDENCE_KIND = "postgamma.runtime-events"
NORMAL_CONNECTIONS = 1000
TRACE_CONNECTIONS = 32
EVENT_QUEUE_CAPACITY = 64
MINIMUM_NOFILE_LIMIT = 8192
MAX_BASELINE_FDS_PER_CONNECTION = 5
REQUIRED_MARKER_VALUES = {
    "diagnostics": "17",
    "object_diagnostics": "true",
    "parameter_status": "true",
    "precedence": "true",
    "settings_fail_closed": "true",
    "request_notice_override": "true",
    "idle_notification_poll": "true",
    "idle_notification_dispatch": "true",
    "exact_once": "true",
    "routed_log": "true",
    "callback_host_thread": "true",
    "reentrant_status": "14",
    "overflow": "true",
    "shared_waitable": "true",
    "routing": "o1",
    "startup_polled": "1",
    "event_capacity": str(EVENT_QUEUE_CAPACITY),
    "request_count": "12",
    "completed_count": "10",
    "failed_count": "2",
    "phase": "closed",
}
EXPECTED_FATAL_FRAGMENTS = (
    'unrecognized configuration parameter "postgamma_missing_guc"',
    'parameter "max_connections" cannot be changed without restarting',
)


class EventsCheckError(RuntimeError):
    """The diagnostics, settings, or event contract was violated."""


def fields(line: str) -> dict[str, str]:
    return dict(re.findall(r"([a-z_]+)=([^\s]+)", line))


def parse_marker(
    stdout: str, *, expected_connections: int, trace: bool
) -> dict[str, str | int]:
    lines = [line for line in stdout.splitlines() if line.startswith(f"{MARKER} ")]
    if len(lines) != 1:
        raise EventsCheckError(f"expected one {MARKER} line, found {len(lines)}")
    values = fields(lines[0])
    for name, expected in REQUIRED_MARKER_VALUES.items():
        if values.get(name) != expected:
            raise EventsCheckError(f"event marker field {name} is not {expected}")
    expected_trace = "true" if trace else "false"
    if values.get("trace") != expected_trace:
        raise EventsCheckError(f"event marker trace is not {expected_trace}")
    try:
        connections = int(values.get("connections", ""))
        fd_growth = int(values.get("fd_growth", ""))
        overflow_reported = int(values.get("overflow_reported", ""))
    except ValueError as exc:
        raise EventsCheckError("event marker counters are invalid") from exc
    if connections != expected_connections:
        raise EventsCheckError(
            f"event marker connections is not {expected_connections}"
        )
    if fd_growth < 0 or fd_growth > (
        connections * MAX_BASELINE_FDS_PER_CONNECTION
    ):
        raise EventsCheckError(
            f"connection FD growth {fd_growth} exceeds the reviewed baseline"
        )
    if overflow_reported <= 0:
        raise EventsCheckError("event overflow was not reported")
    return {
        **values,
        "connections": connections,
        "fd_growth": fd_growth,
        "overflow_reported": overflow_reported,
    }


def parse_runtime(stderr: str, *, expected_connections: int) -> dict[str, str | int]:
    lines = [line for line in stderr.splitlines() if "POSTGAMMA_RUNTIME" in line]
    if len(lines) != 1:
        raise EventsCheckError(
            f"expected one POSTGAMMA_RUNTIME line, found {len(lines)}"
        )
    values = fields(lines[0])
    required = {
        "backend_model": "thread",
        "provider": "pooled",
        "threads_started": "true",
        "role_process_launches": "0",
        "forbidden_process_launch_attempts": "0",
        "unsupported_role_requests": "0",
        "role_threads_active": "0",
        "runnable_sessions": "0",
        "running_quantums": "0",
        "pinned_sessions": "0",
        "blocked_sessions": "0",
        "execution_tokens_active": "0",
        "execution_token_rejections": "0",
    }
    for name, expected in required.items():
        if values.get(name) != expected:
            raise EventsCheckError(f"runtime field {name} is not {expected}")
    numeric_names = (
        "host_pid",
        "client_threads_started",
        "client_threads_peak",
        "role_completions",
        "pooled_worker_threads",
        "client_quantums",
        "quantum_yields",
        "carrier_migrations",
        "running_quantums_peak",
        "execution_tokens_peak",
        "execution_token_budget",
    )
    try:
        numeric = {name: int(values.get(name, "")) for name in numeric_names}
    except ValueError as exc:
        raise EventsCheckError("runtime counters are invalid") from exc
    if numeric["host_pid"] <= 0:
        raise EventsCheckError("runtime host process identity is invalid")
    if (
        numeric["client_threads_started"] != 4
        or numeric["pooled_worker_threads"] != 4
        or not 1 <= numeric["client_threads_peak"] <= 4
        or not 1 <= numeric["running_quantums_peak"] <= 4
    ):
        raise EventsCheckError("runtime did not preserve the four-worker bound")
    if numeric["role_completions"] < expected_connections:
        raise EventsCheckError("runtime did not complete every scale connection")
    if (
        numeric["client_quantums"] < expected_connections * 2
        or numeric["quantum_yields"] < expected_connections
        or numeric["carrier_migrations"] == 0
    ):
        raise EventsCheckError("runtime did not exercise resumable idle sessions")
    if (
        numeric["execution_token_budget"] != 4
        or not 1 <= numeric["execution_tokens_peak"] <= 4
    ):
        raise EventsCheckError("runtime execution-token budget is invalid")
    if "PANIC:" in stderr:
        raise EventsCheckError("event run emitted a backend panic")
    fatal_lines = [line for line in stderr.splitlines() if "FATAL:" in line]
    if len(fatal_lines) != len(EXPECTED_FATAL_FRAGMENTS) or any(
        not any(fragment in line for line in fatal_lines)
        for fragment in EXPECTED_FATAL_FRAGMENTS
    ):
        raise EventsCheckError("event run emitted an unexpected fatal diagnostic")
    return {**values, **numeric}


def source_function(content: str, name: str) -> str:
    start, end = function_span(sanitize_c(content), name, 1)
    return content[start:end]


def audit_connection_open_order(content: str) -> None:
    connection_open = source_function(content, "pgm_connection_open")
    register_at = connection_open.find(
        "postgamma_public_event_register_connection("
    )
    notify_at = connection_open.find("postgamma_private_libpq_set_notify(")
    if register_at < 0 or notify_at < 0 or register_at > notify_at:
        raise EventsCheckError(
            "connection notify is installed before event registration"
        )


def audit_event_routing(
    event_source: Path,
    connection_source: Path,
    runtime_source: Path,
    manifest: Path,
) -> dict[str, object]:
    event_content = event_source.read_text(encoding="utf-8")
    connection_content = connection_source.read_text(encoding="utf-8")
    runtime_content = runtime_source.read_text(encoding="utf-8")
    manifest_document = json.loads(manifest.read_text(encoding="utf-8"))
    create = source_function(event_content, "postgamma_public_event_router_create")
    register = source_function(
        event_content, "postgamma_public_event_register_connection"
    )
    notify = source_function(
        event_content, "postgamma_public_event_notify_connection"
    )
    collect = source_function(event_content, "collect_pending_locked")
    pump = source_function(event_content, "pump_ready_connections")
    if event_content.count("postgamma_wake_target_create(") != 1:
        raise EventsCheckError("event router must have one waitable creation call")
    if create.count("postgamma_wake_target_create(") != 1:
        raise EventsCheckError("instance event router does not create one waitable")
    if any(token in register for token in ("postgamma_wake_target_create(", "eventfd(", "pipe(")):
        raise EventsCheckError("connection registration creates a waitable")
    if (
        "atomic_compare_exchange_weak_explicit" not in notify
        or "pending_head" not in notify
        or "atomic_exchange_explicit" not in collect
    ):
        raise EventsCheckError("event readiness does not use the atomic pending stack")
    if "router->connections" in pump or "take_ready_connection" not in pump:
        raise EventsCheckError("event pumping scans the connection registry")
    if "pq_buffer_remaining_data() > 0" not in runtime_content:
        raise EventsCheckError("idle quantum seam ignores buffered protocol input")
    if "saved_idle_session_deadline_ns" not in runtime_content:
        raise EventsCheckError("idle quantum seam does not preserve its deadline")
    audit_connection_open_order(connection_content)
    command_edits = [
        edit
        for edit in manifest_document.get("edits", [])
        if edit.get("id") == "quantum-command-read-yield"
    ]
    if len(command_edits) != 1 or "DoingCommandRead" in command_edits[0].get(
        "replacement", ""
    ):
        raise EventsCheckError("quantum seam depends on an untransformed role variable")
    return {
        "instance_waitable_creation_sites": 1,
        "per_connection_waitable_creation_sites": 0,
        "atomic_pending_stack": True,
        "ready_queue_routing": "o1",
        "connection_registry_scans_on_pump": 0,
        "notify_installed_after_registration": True,
        "buffered_protocol_input_guard": True,
        "idle_deadline_preserved": True,
    }


def prepend_library_path(environment: dict[str, str], path: Path) -> None:
    values = [str(path)]
    if environment.get("LD_LIBRARY_PATH"):
        values.append(environment["LD_LIBRARY_PATH"])
    environment["LD_LIBRARY_PATH"] = os.pathsep.join(values)


@contextmanager
def raised_nofile_limit() -> Iterator[dict[str, int]]:
    soft, hard = resource.getrlimit(resource.RLIMIT_NOFILE)
    hard_value = MINIMUM_NOFILE_LIMIT if hard == resource.RLIM_INFINITY else hard
    if hard_value < MINIMUM_NOFILE_LIMIT:
        raise EventsCheckError(
            f"RLIMIT_NOFILE hard limit {hard_value} cannot run the 1000-session gate"
        )
    target = max(soft, MINIMUM_NOFILE_LIMIT)
    changed = target != soft
    if changed:
        resource.setrlimit(resource.RLIMIT_NOFILE, (target, hard))
    try:
        yield {"original_soft": soft, "active_soft": target, "hard": hard_value}
    finally:
        if changed:
            resource.setrlimit(resource.RLIMIT_NOFILE, (soft, hard))


def write_text(path: Path, content: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content, encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--make", default="make")
    parser.add_argument("--jobs", type=int, default=1)
    parser.add_argument("--postgres-build", required=True, type=Path)
    parser.add_argument("--library", required=True, type=Path)
    parser.add_argument("--driver", required=True, type=Path)
    parser.add_argument("--event-source", required=True, type=Path)
    parser.add_argument("--connection-source", required=True, type=Path)
    parser.add_argument("--runtime-source", required=True, type=Path)
    parser.add_argument("--quantum-manifest", required=True, type=Path)
    parser.add_argument("--work-root", required=True, type=Path)
    parser.add_argument("--stdout", required=True, type=Path)
    parser.add_argument("--stderr", required=True, type=Path)
    parser.add_argument("--trace", required=True, type=Path)
    parser.add_argument("--strace", default="strace")
    parser.add_argument("--input", action="append", default=[], type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()

    generated = (args.stdout, args.stderr, args.trace, args.output)
    for path in generated:
        path.resolve().parent.mkdir(parents=True, exist_ok=True)
        path.resolve().unlink(missing_ok=True)
    try:
        postgres_build = args.postgres_build.resolve(strict=True)
        library = args.library.resolve(strict=True)
        driver = args.driver.resolve(strict=True)
        event_source = args.event_source.resolve(strict=True)
        connection_source = args.connection_source.resolve(strict=True)
        runtime_source = args.runtime_source.resolve(strict=True)
        quantum_manifest = args.quantum_manifest.resolve(strict=True)
        inputs = [path.resolve(strict=True) for path in args.input]
        work_root = args.work_root.resolve()
        work_root.mkdir(parents=True, exist_ok=True)
        routing = audit_event_routing(
            event_source, connection_source, runtime_source, quantum_manifest
        )
        with tempfile.TemporaryDirectory(
            prefix="runtime_events-events-", dir=work_root
        ) as temporary, raised_nofile_limit() as nofile:
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
            prepend_library_path(environment, library.parent)
            environment["LC_ALL"] = "C"
            base_command = [
                str(driver),
                str(temporary_path / "data"),
                str(prefix / "bin" / "postgres"),
                str(prefix),
                "create",
            ]
            normal = run_checked(
                base_command, environment=environment, timeout=300.0, check=False
            )
            write_text(args.stdout.resolve(), normal.stdout)
            write_text(args.stderr.resolve(), normal.stderr)
            if normal.returncode != 0:
                raise EventsCheckError(
                    f"normal event driver failed ({normal.returncode})\n"
                    f"stdout:\n{normal.stdout}\nstderr:\n{normal.stderr}"
                )
            normal_marker = parse_marker(
                normal.stdout, expected_connections=NORMAL_CONNECTIONS, trace=False
            )
            normal_runtime = parse_runtime(
                normal.stderr, expected_connections=NORMAL_CONNECTIONS
            )
            trace_command = [
                args.strace,
                "-f",
                "-qq",
                "-o",
                str(args.trace.resolve()),
                "-e",
                "trace=process,signal,network,chdir,fchdir,umask,setitimer",
                str(driver),
                str(temporary_path / "trace-data"),
                str(prefix / "bin" / "postgres"),
                str(prefix),
                "create",
                "trace",
            ]
            traced = run_checked(
                trace_command, environment=environment, timeout=180.0, check=False
            )
            if traced.returncode != 0:
                raise EventsCheckError(
                    f"traced event driver failed ({traced.returncode})\n"
                    f"stdout:\n{traced.stdout}\nstderr:\n{traced.stderr}"
                )
            trace_marker = parse_marker(
                traced.stdout, expected_connections=TRACE_CONNECTIONS, trace=True
            )
            trace_runtime = parse_runtime(
                traced.stderr, expected_connections=TRACE_CONNECTIONS
            )
            trace_text = args.trace.resolve().read_text(encoding="utf-8")
            host_safety = audit_trace(trace_text, driver)
        document = {
            "schema_version": 1,
            "kind": EVIDENCE_KIND,
            "status": "pass",
            "postgresql_major": 19,
            "library": str(library),
            "driver": str(driver),
            "normal": {"marker": normal_marker, "runtime": normal_runtime},
            "trace": {"marker": trace_marker, "runtime": trace_runtime},
            "event_routing": routing,
            "host_safety": host_safety,
            "resource_limit": nofile,
            "scale": {
                "logical_connections": NORMAL_CONNECTIONS,
                "executor_workers": 4,
                "maximum_observed_fd_growth": normal_marker["fd_growth"],
                "reviewed_fd_baseline_per_connection": (
                    MAX_BASELINE_FDS_PER_CONNECTION
                ),
                "public_waitables_per_instance": 1,
                "public_waitables_per_connection": 0,
            },
            "inputs": input_identity(
                [
                    *inputs,
                    library,
                    event_source,
                    runtime_source,
                    quantum_manifest,
                ]
            ),
        }
        write_json(args.output.resolve(), document)
    except (
        CSourceProbeError,
        EventsCheckError,
        LifecycleCheckError,
        OSError,
        PublicApiCheckError,
        json.JSONDecodeError,
        ValueError,
    ) as exc:
        parser.error(str(exc))
    print(
        "events and diagnostics evidence: pass "
        "(1000 logical connections, one routed waitable, host-thread callbacks)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
