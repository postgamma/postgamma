#!/usr/bin/env python3
"""Validate the bounded session/executor product contract."""

from __future__ import annotations

import argparse
import re
import tempfile
from pathlib import Path

from check_embedded_lifecycle import (
    input_identity,
    installed_prefix,
    run_checked,
    runtime_environment,
    write_json,
)
from check_embedded_public_api import PublicApiCheckError, audit_trace


MARKER = "POSTGAMMA_SESSION_EXECUTOR"


class SessionExecutorCheckError(RuntimeError):
    """The bounded session/executor contract was not met."""


def parse_values(line: str) -> dict[str, str]:
    return dict(re.findall(r"([a-z_]+)=([^\s]+)", line))


def parse_marker(stdout: str) -> dict[str, int | str]:
    lines = [line for line in stdout.splitlines() if MARKER in line]
    if len(lines) != 1:
        raise SessionExecutorCheckError(
            f"expected one {MARKER} line, found {len(lines)}"
        )
    values = parse_values(lines[0])
    required = {
        "connections": "1000",
        "request_storm": "1000",
        "holder_progress": "true",
        "saturated_waiters": "4",
        "pinning_cliff": "true",
        "pinned_transactions": "4",
        "queued_at_cliff": "true",
        "parallel_query": "true",
        "executor_workers": "4",
        "request_threads": "0",
        "phase": "closed",
    }
    for name, expected in required.items():
        if values.get(name) != expected:
            raise SessionExecutorCheckError(f"marker {name} is not {expected}")
    try:
        idle_growth = int(values.get("idle_thread_growth", ""))
        request_growth = int(values.get("request_thread_growth", ""))
        idle_fd_growth = int(values.get("idle_fd_growth", ""))
        nofile_soft = int(values.get("nofile_soft", ""))
    except ValueError as exc:
        raise SessionExecutorCheckError("resource growth is not numeric") from exc
    if idle_growth < 0 or idle_growth > 2:
        raise SessionExecutorCheckError("idle sessions grew native threads")
    if request_growth < 0 or request_growth > 2:
        raise SessionExecutorCheckError("request storm grew native threads")
    if idle_fd_growth <= 0 or nofile_soft < 16384:
        raise SessionExecutorCheckError(
            "descriptor use was not disclosed under the test envelope"
        )
    return {
        **values,
        "idle_thread_growth": idle_growth,
        "request_thread_growth": request_growth,
        "idle_fd_growth": idle_fd_growth,
        "nofile_soft": nofile_soft,
    }


def parse_runtime(stderr: str) -> dict[str, int | str]:
    lines = [line for line in stderr.splitlines() if "POSTGAMMA_RUNTIME" in line]
    if len(lines) != 1:
        raise SessionExecutorCheckError(
            f"expected one POSTGAMMA_RUNTIME line, found {len(lines)}"
        )
    values = parse_values(lines[0])
    required = {
        "backend_model": "thread",
        "provider": "pooled",
        "threads_started": "true",
        "role_process_launches": "0",
        "forbidden_process_launch_attempts": "0",
        "unsupported_role_requests": "0",
    }
    for name, expected in required.items():
        if values.get(name) != expected:
            raise SessionExecutorCheckError(
                f"runtime marker {name} is not {expected}"
            )
    names = (
        "client_threads_started",
        "client_threads_peak",
        "parallel_threads_started",
        "role_completions",
        "role_threads_active",
        "pooled_worker_threads",
        "client_quantums",
        "quantum_yields",
        "carrier_migrations",
        "work_steals",
        "runnable_sessions",
        "runnable_sessions_peak",
        "running_quantums",
        "running_quantums_peak",
        "pinned_sessions",
        "pinned_sessions_peak",
        "blocked_sessions",
        "blocked_sessions_peak",
        "execution_tokens_active",
        "execution_tokens_peak",
        "execution_token_budget",
        "execution_token_rejections",
        "queue_wait_ns_total",
        "queue_wait_ns_max",
    )
    try:
        numbers = {name: int(values.get(name, "")) for name in names}
    except ValueError as exc:
        raise SessionExecutorCheckError("runtime telemetry is not numeric") from exc
    if (
        numbers["client_threads_started"] != 4
        or numbers["pooled_worker_threads"] != 4
        or numbers["client_threads_peak"] > 4
        or numbers["running_quantums_peak"] != 4
    ):
        raise SessionExecutorCheckError("client executor was not bounded at four")
    if (
        numbers["client_quantums"] < 2000
        or numbers["quantum_yields"] < 2000
        or numbers["carrier_migrations"] < 1000
    ):
        raise SessionExecutorCheckError("logical sessions did not migrate by quantum")
    if numbers["runnable_sessions_peak"] < 900:
        raise SessionExecutorCheckError("request storm did not saturate the runnable queue")
    if numbers["pinned_sessions_peak"] != 4:
        raise SessionExecutorCheckError(
            "four-worker transaction pinning cliff was not observed"
        )
    if numbers["blocked_sessions_peak"] < 3:
        raise SessionExecutorCheckError("saturated lock waiters were not observed")
    if numbers["parallel_threads_started"] < 1:
        raise SessionExecutorCheckError("parallel workers were not exercised")
    if (
        numbers["execution_token_budget"] != 4
        or numbers["execution_tokens_peak"] != 4
    ):
        raise SessionExecutorCheckError("unified execution budget was not saturated")
    for name in (
        "role_threads_active",
        "runnable_sessions",
        "running_quantums",
        "pinned_sessions",
        "blocked_sessions",
        "execution_tokens_active",
    ):
        if numbers[name] != 0:
            raise SessionExecutorCheckError(f"runtime marker {name} leaked")
    if numbers["queue_wait_ns_total"] <= 0 or numbers["queue_wait_ns_max"] <= 0:
        raise SessionExecutorCheckError("executor queue latency was not recorded")
    if "FATAL:" in stderr or "PANIC:" in stderr:
        raise SessionExecutorCheckError("session/executor gate emitted FATAL or PANIC")
    return {**values, **numbers}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--make", default="make")
    parser.add_argument("--jobs", type=int, default=1)
    parser.add_argument("--postgres-build", required=True, type=Path)
    parser.add_argument("--driver", required=True, type=Path)
    parser.add_argument("--work-root", required=True, type=Path)
    parser.add_argument("--stdout", required=True, type=Path)
    parser.add_argument("--stderr", required=True, type=Path)
    parser.add_argument("--trace", required=True, type=Path)
    parser.add_argument("--input", action="append", default=[], type=Path)
    parser.add_argument("--strace", default="strace")
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()

    generated = (args.stdout, args.stderr, args.trace, args.output)
    for path in generated:
        path.resolve().parent.mkdir(parents=True, exist_ok=True)
        path.resolve().unlink(missing_ok=True)
    try:
        postgres_build = args.postgres_build.resolve(strict=True)
        driver = args.driver.resolve(strict=True)
        inputs = [path.resolve(strict=True) for path in args.input]
        work_root = args.work_root.resolve()
        work_root.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="session-executor-", dir=work_root) as temp:
            temporary = Path(temp)
            install_root = temporary / "install"
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
            command = [
                args.strace,
                "-f",
                "-qq",
                "-o",
                str(args.trace.resolve()),
                "-e",
                "trace=process,signal,network,chdir,fchdir,umask,setitimer",
                str(driver),
                str(temporary / "data"),
                str(prefix / "bin" / "postgres"),
                str(prefix),
                "create",
            ]
            completed = run_checked(
                command, environment=environment, timeout=600.0, check=False
            )
            args.stdout.resolve().write_text(completed.stdout, encoding="utf-8")
            args.stderr.resolve().write_text(completed.stderr, encoding="utf-8")
            if completed.returncode != 0:
                raise SessionExecutorCheckError(
                    f"session executor driver failed ({completed.returncode})\n"
                    f"stdout:\n{completed.stdout}\nstderr:\n{completed.stderr}"
                )
            marker = parse_marker(completed.stdout)
            runtime = parse_runtime(completed.stderr)
            trace_text = args.trace.resolve().read_text(encoding="utf-8")
            host_safety = audit_trace(trace_text, driver)
            if host_safety["thread_clone_calls"] > 32:
                raise SessionExecutorCheckError(
                    "native thread creation exceeded the fixed executor envelope"
                )
        write_json(
            args.output.resolve(),
            {
                "schema_version": 1,
                "kind": "postgamma.session-executor",
                "status": "pass",
                "marker": marker,
                "runtime": runtime,
                "host_safety": host_safety,
                "inputs": input_identity(inputs),
            },
        )
    except (OSError, PublicApiCheckError, SessionExecutorCheckError) as exc:
        parser.error(str(exc))
    print(
        "session executor evidence: pass "
        "(1,000 sessions, bounded workers, holder progress, parallel budget)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
