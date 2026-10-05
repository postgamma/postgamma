#!/usr/bin/env python3
"""Exercise real PostgreSQL role threads for isolation and concurrency."""

from __future__ import annotations

import argparse
import json
import random
import re
import shutil
import subprocess
import sys
import tempfile
import time
from collections import Counter
from pathlib import Path
from typing import Any, Mapping, Sequence

from check_threaded_runtime import (
    QueryResult,
    ThreadedRuntimeError,
    available_port,
    installed_prefix,
    linux_process_evidence,
    psql_arguments,
    run_checked,
    run_concurrent_queries,
    run_query,
    server_environment,
    validate_telemetry_log,
)


PROFILES = ("check", "stress", "soak")
MAIN_WORKLOAD_FD_GROWTH_LIMIT = 64
REPEAT_CHURN_FD_GROWTH_LIMIT = 8


class IsolationError(RuntimeError):
    """A threaded PostgreSQL isolation invariant failed."""


def require_query(result: QueryResult, label: str) -> str:
    if result.returncode != 0:
        raise IsolationError(f"{label} failed: {result.stderr}")
    return result.stdout


def process_fd_count(pid: int) -> int | None:
    directory = Path("/proc") / str(pid) / "fd"
    if not directory.is_dir():
        return None
    return sum(1 for _path in directory.iterdir())


def process_fd_summary(pid: int) -> dict[str, int]:
    directory = Path("/proc") / str(pid) / "fd"
    if not directory.is_dir():
        return {}
    categories: Counter[str] = Counter()
    for path in directory.iterdir():
        try:
            target = path.readlink().as_posix()
        except OSError:
            continue
        if target.startswith("socket:"):
            category = "socket"
        elif target.startswith("pipe:"):
            category = "pipe"
        elif target.startswith("anon_inode:"):
            category = target
        elif target.startswith("/"):
            category = "file"
        else:
            category = "other"
        categories[category] += 1
    return dict(sorted(categories.items()))


def process_file_samples(pid: int, limit: int = 64) -> list[dict[str, Any]]:
    directory = Path("/proc") / str(pid) / "fd"
    if not directory.is_dir():
        return []
    targets: Counter[str] = Counter()
    for path in directory.iterdir():
        try:
            target = path.readlink().as_posix()
        except OSError:
            continue
        if target.startswith("/"):
            targets[target] += 1
    return [
        {"target": target, "count": count}
        for target, count in sorted(
            targets.items(), key=lambda item: (-item[1], item[0])
        )[:limit]
    ]


def settled_fd_count(pid: int, timeout: float = 10.0) -> int | None:
    deadline = time.monotonic() + timeout
    previous: int | None = None
    stable_samples = 0
    while time.monotonic() < deadline:
        current = process_fd_count(pid)
        if current is None:
            return None
        if current == previous:
            stable_samples += 1
            if stable_samples >= 5:
                return current
        else:
            previous = current
            stable_samples = 0
        time.sleep(0.1)
    return process_fd_count(pid)


def validate_fd_growth(
    before: int | None,
    after: int | None,
    limit: int,
    label: str,
    summary: Mapping[str, int],
) -> None:
    if before is not None and after is not None and after > before + limit:
        raise IsolationError(
            f"{label} grew postmaster file descriptors from {before} to {after} "
            f"(limit +{limit}): {dict(summary)}"
        )


def assert_thread_topology(postmaster_pid: int, minimum_threads: int = 2) -> int | None:
    native_threads, children = linux_process_evidence(postmaster_pid)
    if children:
        raise IsolationError(
            "PostgreSQL role process children were observed: "
            + ", ".join(str(pid) for pid in sorted(children))
        )
    if native_threads is not None and native_threads < minimum_threads:
        raise IsolationError(
            f"expected at least {minimum_threads} native threads, found {native_threads}"
        )
    return native_threads


def isolation_sql(identifier: int, work_mem_kb: int, barrier_epoch: float) -> str:
    role = f"postgamma_isolation_role_{identifier}"
    marker = f"session-{identifier}"
    return (
        "BEGIN; "
        f"SET LOCAL application_name='{marker}'; "
        f"SET LOCAL work_mem='{work_mem_kb}kB'; "
        "SET LOCAL search_path=pg_temp,public; "
        f"SET LOCAL ROLE {role}; "
        "CREATE TEMP TABLE session_marker(value integer) ON COMMIT DROP; "
        f"INSERT INTO session_marker VALUES ({identifier}); "
        f"PREPARE session_statement AS SELECT {identifier}; "
        f"DECLARE session_cursor CURSOR FOR SELECT {identifier}; "
        f"SELECT pg_advisory_xact_lock({identifier}); "
        "SELECT pg_sleep(GREATEST(0, "
        f"{barrier_epoch:.6f} - extract(epoch FROM clock_timestamp()))); "
        "EXECUTE session_statement; "
        "FETCH ALL FROM session_cursor; "
        "SELECT CASE WHEN "
        f"current_setting('application_name')='{marker}' AND "
        f"pg_size_bytes(current_setting('work_mem'))={work_mem_kb * 1024} AND "
        f"current_role='{role}' AND "
        f"(SELECT value FROM session_marker)={identifier} "
        f"THEN 'isolation-ok-{identifier}' ELSE 'isolation-bad-{identifier}' END; "
        "ROLLBACK;"
    )


def validate_session_isolation(
    base_arguments: Sequence[str],
    environment: Mapping[str, str],
    session_count: int,
) -> dict[str, int]:
    role_sql = "; ".join(
        f"CREATE ROLE postgamma_isolation_role_{identifier} NOLOGIN"
        for identifier in range(session_count)
    )
    setup = run_query(base_arguments, role_sql + ";", environment, timeout=60.0)
    require_query(setup, "session isolation setup")
    barrier_epoch = time.time() + 2.0
    queries = {
        f"session_{identifier}": isolation_sql(
            identifier, 1024 + identifier * 64, barrier_epoch
        )
        for identifier in range(session_count)
    }
    results = run_concurrent_queries(
        base_arguments, queries, environment, timeout=max(30.0, session_count * 1.5)
    )
    for identifier in range(session_count):
        result = results[f"session_{identifier}"]
        require_query(result, f"isolation session {identifier}")
        output_lines = result.stdout.splitlines()
        if (
            f"isolation-ok-{identifier}" not in output_lines
            or any(line.startswith("isolation-bad-") for line in output_lines)
        ):
            raise IsolationError(
                f"session {identifier} observed another session's state: {result.stdout}"
            )
    return {"sessions": session_count}


def validate_connection_churn(
    base_arguments: Sequence[str],
    environment: Mapping[str, str],
    batches: int,
    width: int,
    abrupt_disconnects: bool = True,
) -> dict[str, int]:
    completed = 0
    for batch in range(batches):
        queries = {
            f"churn_{batch}_{index}": (
                f"SET application_name='churn-{batch}-{index}'; "
                f"SELECT {batch * width + index};"
            )
            for index in range(width)
        }
        results = run_concurrent_queries(
            base_arguments, queries, environment, timeout=30.0
        )
        for name, result in results.items():
            require_query(result, name)
        completed += len(results)

    abrupt_count = min(width, 4) if abrupt_disconnects else 0
    clients = []
    for index in range(abrupt_count):
        client_environment = dict(environment)
        client_environment["PGAPPNAME"] = f"postgamma-abrupt-{index}"
        clients.append(
            subprocess.Popen(
                [
                    *base_arguments,
                    "-c",
                    "COPY (SELECT repeat('x',4096) "
                    "FROM generate_series(1,1000000)) TO STDOUT;",
                ],
                env=client_environment,
                text=True,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
        )
    if clients:
        time.sleep(0.25)
        for process in clients:
            process.terminate()
        for process in clients:
            try:
                process.wait(timeout=5.0)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
        deadline = time.monotonic() + 20.0
        while time.monotonic() < deadline:
            active = run_query(
                base_arguments,
                "SELECT count(*) FROM pg_stat_activity "
                "WHERE application_name LIKE 'postgamma-abrupt-%';",
                environment,
            )
            if active.returncode == 0 and active.stdout == "0":
                break
            time.sleep(0.25)
        else:
            raise IsolationError("abruptly disconnected roles did not terminate")
    require_query(
        run_query(base_arguments, "SELECT 42;", environment),
        "post-churn health query",
    )
    return {"completed_connections": completed, "abrupt_disconnects": len(clients)}


def validate_auth_failure_isolation(
    base_arguments: Sequence[str], environment: Mapping[str, str]
) -> dict[str, int]:
    invalid_role = "postgamma_role_that_must_not_exist"
    arguments = [
        *base_arguments[:-1],
        "-U",
        invalid_role,
        base_arguments[-1],
        "-c",
        "SELECT 1;",
    ]
    result = subprocess.run(
        arguments,
        env=environment,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        timeout=15.0,
    )
    if result.returncode == 0 or "does not exist" not in result.stderr:
        raise IsolationError(
            "authentication failure did not remain isolated: " + result.stderr
        )
    require_query(
        run_query(base_arguments, "SELECT 86;", environment),
        "post-authentication-failure health query",
    )
    return {"rejected_connections": 1}


def validate_error_isolation(
    base_arguments: Sequence[str], environment: Mapping[str, str]
) -> dict[str, int]:
    results = run_concurrent_queries(
        base_arguments,
        {
            "statement_timeout": (
                "SET statement_timeout=100; SELECT pg_sleep(10);"
            ),
            "division_by_zero": "SELECT 1/0;",
            "subtransaction_recovery": (
                "DO $$ BEGIN PERFORM 1/0; EXCEPTION WHEN division_by_zero "
                "THEN NULL; END $$; SELECT 'subtransaction-ok';"
            ),
            "independent": "SELECT 'error-isolation-ok' FROM pg_sleep(0.2);",
        },
        environment,
        timeout=15.0,
    )
    timeout_result = results["statement_timeout"]
    if timeout_result.returncode == 0 or "statement timeout" not in timeout_result.stderr:
        raise IsolationError("statement timeout did not abort only its own session")
    division_result = results["division_by_zero"]
    if division_result.returncode == 0 or "division by zero" not in division_result.stderr:
        raise IsolationError("ERROR did not abort only its own session")
    recovered = require_query(
        results["subtransaction_recovery"], "subtransaction error recovery"
    )
    if "subtransaction-ok" not in recovered.splitlines():
        raise IsolationError("session did not recover from a subtransaction ERROR")
    independent = require_query(results["independent"], "independent error session")
    if "error-isolation-ok" not in independent.splitlines():
        raise IsolationError("an ERROR contaminated an independent session")
    require_query(
        run_query(base_arguments, "SELECT 87;", environment),
        "post-error health query",
    )
    return {"expected_errors": 2, "recovered_sessions": 2}


def validate_mvcc_isolation(
    base_arguments: Sequence[str], environment: Mapping[str, str]
) -> dict[str, int]:
    table = "postgamma_mvcc_isolation"
    require_query(
        run_query(
            base_arguments,
            f"DROP TABLE IF EXISTS {table}; CREATE TABLE {table}(value integer); "
            f"INSERT INTO {table} VALUES (1);",
            environment,
        ),
        "MVCC setup",
    )
    writer_environment = dict(environment)
    writer_environment["PGAPPNAME"] = "postgamma-mvcc-writer"
    writer = subprocess.Popen(
        [
            *base_arguments,
            "-c",
            f"BEGIN; UPDATE {table} SET value=2; SELECT pg_sleep(3); "
            "COMMIT; SELECT 'writer-ok';",
        ],
        env=writer_environment,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    try:
        deadline = time.monotonic() + 10.0
        while time.monotonic() < deadline:
            lock = run_query(
                base_arguments,
                "SELECT count(*) FROM pg_locks l "
                "JOIN pg_stat_activity a ON a.pid=l.pid "
                "WHERE a.application_name='postgamma-mvcc-writer' "
                f"AND l.relation='{table}'::regclass "
                "AND l.mode='RowExclusiveLock' AND l.granted;",
                environment,
            )
            if lock.returncode == 0 and lock.stdout == "1":
                break
            time.sleep(0.02)
        else:
            raise IsolationError("MVCC writer did not publish its row-exclusive lock")
        reader = run_query(
            base_arguments,
            f"SELECT CASE WHEN value=1 THEN 'mvcc-ok' ELSE 'mvcc-bad' END "
            f"FROM {table};",
            environment,
        )
        reader_output = require_query(reader, "MVCC concurrent reader")
        if reader_output != "mvcc-ok":
            raise IsolationError(
                "reader observed another session's uncommitted value: " + reader_output
            )
        stdout, stderr = writer.communicate(timeout=10.0)
        if writer.returncode != 0 or "writer-ok" not in stdout.splitlines():
            raise IsolationError("MVCC writer failed: " + stdout + stderr)
    finally:
        if writer.poll() is None:
            writer.terminate()
            try:
                writer.wait(timeout=2.0)
            except subprocess.TimeoutExpired:
                writer.kill()
                writer.wait()
    committed = require_query(
        run_query(base_arguments, f"SELECT value FROM {table};", environment),
        "MVCC committed value",
    )
    if committed != "2":
        raise IsolationError("writer commit was not visible after completion")
    return {"writers": 1, "concurrent_readers": 1}


def wait_for_cancel_target(
    base_arguments: Sequence[str], environment: Mapping[str, str], application_name: str
) -> int:
    deadline = time.monotonic() + 10.0
    while time.monotonic() < deadline:
        result = run_query(
            base_arguments,
            "SELECT pid FROM pg_stat_activity WHERE application_name="
            f"'{application_name}' LIMIT 1;",
            environment,
        )
        if result.returncode == 0 and result.stdout:
            return int(result.stdout.splitlines()[-1])
        time.sleep(0.02)
    raise IsolationError("cancel target did not publish its compatibility PID")


def validate_cancel_isolation(
    base_arguments: Sequence[str], environment: Mapping[str, str]
) -> dict[str, int]:
    application_name = "postgamma-cancel-target"
    target_environment = dict(environment)
    target_environment["PGAPPNAME"] = application_name
    target = subprocess.Popen(
        [
            *base_arguments,
            "-c",
            "SELECT pg_sleep(30);",
        ],
        env=target_environment,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    try:
        compatibility_pid = wait_for_cancel_target(
            base_arguments, environment, application_name
        )
        cancel = run_query(
            base_arguments,
            f"SELECT pg_cancel_backend({compatibility_pid});",
            environment,
        )
        if cancel.returncode != 0 or not cancel.stdout.endswith("t"):
            raise IsolationError(f"could not cancel target role: {cancel.stderr}")
        independent = run_query(
            base_arguments, "SELECT pg_sleep(0.1), 84;", environment
        )
        require_query(independent, "independent session during cancel")
        stdout, stderr = target.communicate(timeout=10.0)
        if target.returncode == 0 or "canceling statement" not in stderr:
            raise IsolationError(
                "target statement was not canceled independently: " + stdout + stderr
            )
    finally:
        if target.poll() is None:
            target.terminate()
            try:
                target.wait(timeout=2.0)
            except subprocess.TimeoutExpired:
                target.kill()
                target.wait()
    require_query(
        run_query(base_arguments, "SELECT 85;", environment),
        "post-cancel health query",
    )
    return {"compatibility_pid": compatibility_pid}


def validate_parallel_sessions(
    base_arguments: Sequence[str],
    environment: Mapping[str, str],
    session_count: int,
) -> dict[str, int]:
    require_query(
        run_query(
            base_arguments,
            "CREATE UNLOGGED TABLE postgamma_parallel_isolation AS "
            "SELECT value FROM generate_series(1,3000000) AS value; "
            "ALTER TABLE postgamma_parallel_isolation SET (parallel_workers=2); "
            "ANALYZE postgamma_parallel_isolation;",
            environment,
            timeout=120.0,
        ),
        "parallel isolation setup",
    )
    sql = (
        "SET max_parallel_workers_per_gather=2; "
        "SET min_parallel_table_scan_size=0; "
        "SET parallel_setup_cost=0; SET parallel_tuple_cost=0; "
        "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) "
        "SELECT sum(value) FROM postgamma_parallel_isolation;"
    )
    results = run_concurrent_queries(
        base_arguments,
        {f"parallel_{index}": sql for index in range(session_count)},
        environment,
        timeout=180.0,
    )
    workers = 0
    for name, result in results.items():
        require_query(result, name)
        match = re.search(r"Workers Launched:\s+([1-9][0-9]*)", result.stdout)
        if match is None:
            raise IsolationError(f"{name} did not launch a parallel worker")
        workers += int(match.group(1))
    return {"sessions": session_count, "workers_launched": workers}


def random_statement(random_source: random.Random, identifier: int) -> str:
    work_mem = random_source.randrange(1024, 32768, 64)
    timeout = random_source.choice((0, 1000, 2500))
    return (
        "BEGIN; "
        f"SET LOCAL application_name='stress-{identifier}'; "
        f"SET work_mem='{work_mem}kB'; SET statement_timeout={timeout}; "
        "CREATE TEMP TABLE stress_marker(value integer) ON COMMIT DROP; "
        f"INSERT INTO stress_marker VALUES ({identifier}); "
        f"SELECT CASE WHEN current_setting('application_name')='stress-{identifier}' "
        f"AND (SELECT value FROM stress_marker)={identifier} "
        f"THEN 'stress-ok-{identifier}' ELSE format("
        f"'stress-bad-{identifier}:app=%s,value=%s', "
        "current_setting('application_name'), "
        "(SELECT value::text FROM stress_marker)) END; COMMIT;"
    )


def validate_random_stress(
    base_arguments: Sequence[str],
    environment: Mapping[str, str],
    seed: int,
    duration_seconds: float,
    width: int,
) -> dict[str, int | float]:
    random_source = random.Random(seed)
    deadline = time.monotonic() + duration_seconds
    iterations = 0
    sessions = 0
    while time.monotonic() < deadline:
        queries = {
            f"stress_{iterations}_{index}": random_statement(
                random_source, iterations * width + index
            )
            for index in range(width)
        }
        results = run_concurrent_queries(
            base_arguments, queries, environment, timeout=60.0
        )
        for index in range(width):
            name = f"stress_{iterations}_{index}"
            result = results[name]
            output = require_query(result, name)
            identifier = iterations * width + index
            if f"stress-ok-{identifier}" not in output.splitlines():
                raise IsolationError(
                    f"{name} observed cross-session state: {output}"
                )
        sessions += len(results)
        iterations += 1
    return {
        "seed": seed,
        "duration_seconds": duration_seconds,
        "iterations": iterations,
        "sessions": sessions,
    }


def run_suite(args: argparse.Namespace) -> dict[str, Any]:
    profile_sizes = {
        "check": {"sessions": 16, "churn_batches": 4, "churn_width": 12, "parallel": 3},
        "stress": {"sessions": 64, "churn_batches": 12, "churn_width": 24, "parallel": 6},
        "soak": {"sessions": 64, "churn_batches": 20, "churn_width": 32, "parallel": 8},
    }
    sizes = profile_sizes[args.profile]
    artifacts = args.artifacts.resolve()
    artifacts.mkdir(parents=True, exist_ok=True)
    report: dict[str, Any] = {
        "schema_version": 1,
        "kind": "postgamma.thread-isolation-report",
        "profile": args.profile,
        "seed": args.seed,
        "workloads": {},
    }
    with tempfile.TemporaryDirectory(prefix="postgamma-isolation-") as temporary:
        temporary_path = Path(temporary)
        install_root = temporary_path / "install"
        run_checked(
            [
                args.make,
                "-C",
                str(args.build.resolve()),
                "install",
                f"DESTDIR={install_root}",
            ],
            timeout=300.0,
        )
        prefix = installed_prefix(install_root)
        environment = server_environment(prefix)
        data_directory = temporary_path / "data"
        socket_directory = temporary_path / "socket"
        log_path = temporary_path / "postmaster.log"
        socket_directory.mkdir()
        port = available_port()
        pg_ctl = prefix / "bin" / "pg_ctl"
        run_checked(
            [
                str(prefix / "bin" / "initdb"),
                "-D",
                str(data_directory),
                "--auth=trust",
                "--encoding=UTF8",
                "--locale=C",
                "--no-sync",
                "--no-instructions",
            ],
            environment=environment,
        )
        options = (
            f"-F -c listen_addresses='' -c unix_socket_directories={socket_directory} "
            f"-p {port} -c max_connections=120 -c max_worker_processes=24 "
            "-c max_parallel_workers=16 -c log_min_messages=log"
        )
        server_running = False
        failure: Exception | None = None
        try:
            run_checked(
                [
                    str(pg_ctl),
                    "-D",
                    str(data_directory),
                    "-l",
                    str(log_path),
                    "-o",
                    options,
                    "-w",
                    "start",
                ],
                environment=environment,
            )
            server_running = True
            postmaster_pid = int(
                (data_directory / "postmaster.pid")
                .read_text(encoding="ascii")
                .splitlines()[0]
            )
            report["postmaster_pid"] = postmaster_pid
            report["native_threads_before"] = assert_thread_topology(postmaster_pid)
            fd_before = settled_fd_count(postmaster_pid)
            fd_summary_before = process_fd_summary(postmaster_pid)
            base_arguments = psql_arguments(prefix, socket_directory, port)
            report["workloads"]["session_isolation"] = validate_session_isolation(
                base_arguments, environment, sizes["sessions"]
            )
            assert_thread_topology(postmaster_pid)
            report["workloads"]["connection_churn"] = validate_connection_churn(
                base_arguments,
                environment,
                sizes["churn_batches"],
                sizes["churn_width"],
            )
            report["workloads"]["authentication_failure"] = (
                validate_auth_failure_isolation(base_arguments, environment)
            )
            report["workloads"]["error_isolation"] = validate_error_isolation(
                base_arguments, environment
            )
            report["workloads"]["mvcc_isolation"] = validate_mvcc_isolation(
                base_arguments, environment
            )
            report["workloads"]["cancel_isolation"] = validate_cancel_isolation(
                base_arguments, environment
            )
            report["workloads"]["parallel_sessions"] = validate_parallel_sessions(
                base_arguments, environment, sizes["parallel"]
            )
            if args.profile in ("stress", "soak"):
                report["workloads"]["random_stress"] = validate_random_stress(
                    base_arguments,
                    environment,
                    args.seed,
                    args.duration_seconds,
                    min(sizes["sessions"], 32),
                )
            report["native_threads_after"] = assert_thread_topology(postmaster_pid)
            fd_after = settled_fd_count(postmaster_pid)
            fd_summary_after = process_fd_summary(postmaster_pid)
            validate_fd_growth(
                fd_before,
                fd_after,
                MAIN_WORKLOAD_FD_GROWTH_LIMIT,
                "main isolation workloads",
                fd_summary_after,
            )
            leak_probe_before = fd_after
            leak_probe = validate_connection_churn(
                base_arguments,
                environment,
                2,
                min(sizes["churn_width"], 12),
                abrupt_disconnects=False,
            )
            leak_probe_after = settled_fd_count(postmaster_pid)
            report["workloads"]["descriptor_leak_probe"] = leak_probe
            report["file_descriptors"] = {
                "initial": fd_before,
                "initial_by_type": fd_summary_before,
                "after_main_workloads": fd_after,
                "after_main_workloads_by_type": fd_summary_after,
                "after_main_workloads_file_samples": process_file_samples(
                    postmaster_pid
                ),
                "leak_probe_before": leak_probe_before,
                "leak_probe_after": leak_probe_after,
                "leak_probe_after_by_type": process_fd_summary(postmaster_pid),
            }
            validate_fd_growth(
                leak_probe_before,
                leak_probe_after,
                REPEAT_CHURN_FD_GROWTH_LIMIT,
                "repeat connection churn",
                process_fd_summary(postmaster_pid),
            )
            run_checked(
                [str(pg_ctl), "-D", str(data_directory), "-m", "fast", "-w", "stop"],
                environment=environment,
            )
            server_running = False
            report["telemetry"] = validate_telemetry_log(log_path)
            report["status"] = "passed"
        except Exception as exc:
            failure = exc
            report["status"] = "failed"
            report["error"] = str(exc)
            setattr(exc, "postgamma_report", report)
        finally:
            if server_running:
                subprocess.run(
                    [
                        str(pg_ctl),
                        "-D",
                        str(data_directory),
                        "-m",
                        "immediate",
                        "-w",
                        "stop",
                    ],
                    env=environment,
                    stdout=subprocess.DEVNULL,
                    stderr=subprocess.DEVNULL,
                    timeout=30.0,
                    check=False,
                )
            if log_path.is_file():
                shutil.copy2(log_path, artifacts / "postmaster.log")
        if failure is not None:
            raise failure
    return report


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", required=True, type=Path)
    parser.add_argument("--make", default="make")
    parser.add_argument("--profile", choices=PROFILES, default="check")
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--duration-seconds", type=float, default=120.0)
    parser.add_argument("--artifacts", required=True, type=Path)
    args = parser.parse_args()
    report_path = args.artifacts.resolve() / "report.json"
    try:
        report = run_suite(args)
    except (OSError, subprocess.SubprocessError, ThreadedRuntimeError, IsolationError) as exc:
        partial_report = getattr(exc, "postgamma_report", None)
        if isinstance(partial_report, dict):
            report = partial_report
        else:
            report = {
                "schema_version": 1,
                "kind": "postgamma.thread-isolation-report",
                "profile": args.profile,
                "seed": args.seed,
                "status": "failed",
                "error": str(exc),
            }
        args.artifacts.resolve().mkdir(parents=True, exist_ok=True)
        report_path.write_text(
            json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8"
        )
        print(f"thread isolation failed: {exc}", file=sys.stderr)
        print(f"reproduce with seed {args.seed}; report={report_path}", file=sys.stderr)
        return 1
    report_path.write_text(
        json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    print(
        f"thread isolation: profile={args.profile}, seed={args.seed}, "
        f"status=passed, report={report_path}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
