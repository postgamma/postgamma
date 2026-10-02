#!/usr/bin/env python3
"""Validate PostgreSQL's generated thread-per-role runtime end to end."""

from __future__ import annotations

import argparse
import json
import os
import re
import shlex
import socket
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Mapping, Sequence


TELEMETRY_MARKER = "POSTGAMMA_RUNTIME"
COMPAT_PID_FIRST = 1_000_000_000
INTEGER_TELEMETRY_FIELDS = (
    "host_pid",
    "role_process_launches",
    "forbidden_process_launch_attempts",
    "unsupported_role_requests",
    "client_threads_started",
    "client_threads_peak",
    "parallel_threads_started",
    "dedicated_threads_started",
    "role_completions",
    "role_threads_active",
)


class ThreadedRuntimeError(RuntimeError):
    """Raised when generated PostgreSQL violates the threaded runtime contract."""


@dataclass(frozen=True)
class QueryResult:
    returncode: int
    elapsed_seconds: float
    stdout: str
    stderr: str


def parse_telemetry(log_text: str) -> dict[str, str]:
    lines = [line for line in log_text.splitlines() if TELEMETRY_MARKER in line]
    if not lines:
        raise ThreadedRuntimeError("PostGamma runtime telemetry is absent")
    payload = lines[-1].split(TELEMETRY_MARKER, 1)[1]
    values = dict(re.findall(r"([a-z_]+)=([^\s]+)", payload))
    required = {
        "backend_model",
        "threads_started",
        *INTEGER_TELEMETRY_FIELDS,
    }
    missing = sorted(required - values.keys())
    if missing:
        raise ThreadedRuntimeError(
            "PostGamma runtime telemetry is missing: " + ", ".join(missing)
        )
    return values


def validate_telemetry(values: Mapping[str, str]) -> dict[str, int]:
    if values["backend_model"] != "thread":
        raise ThreadedRuntimeError("backend_model is not thread")
    if values["threads_started"] != "true":
        raise ThreadedRuntimeError("runtime did not start role threads")
    numbers: dict[str, int] = {}
    for field in INTEGER_TELEMETRY_FIELDS:
        try:
            numbers[field] = int(values[field])
        except ValueError as error:
            raise ThreadedRuntimeError(
                f"telemetry field {field} is not an integer"
            ) from error
        if numbers[field] < 0:
            raise ThreadedRuntimeError(f"telemetry field {field} is negative")
    if numbers["host_pid"] == 0:
        raise ThreadedRuntimeError("telemetry field host_pid is zero")

    for field in (
        "role_process_launches",
        "forbidden_process_launch_attempts",
        "unsupported_role_requests",
        "role_threads_active",
    ):
        if numbers[field] != 0:
            raise ThreadedRuntimeError(f"telemetry field {field} is not zero")
    for field in (
        "client_threads_started",
        "parallel_threads_started",
        "dedicated_threads_started",
    ):
        if numbers[field] == 0:
            raise ThreadedRuntimeError(f"telemetry field {field} is zero")
    if numbers["client_threads_peak"] < 2:
        raise ThreadedRuntimeError("concurrent client role threads were not observed")

    started = sum(
        numbers[field]
        for field in (
            "client_threads_started",
            "parallel_threads_started",
            "dedicated_threads_started",
        )
    )
    if numbers["role_completions"] != started:
        raise ThreadedRuntimeError(
            "role completion count does not match started thread count"
        )
    return numbers


def validate_telemetry_log(log_path: Path) -> dict[str, int]:
    values = parse_telemetry(log_path.read_text(encoding="utf-8"))
    return validate_telemetry(values)


def print_telemetry(numbers: Mapping[str, int], *, prefix: str) -> None:
    print(
        f"{prefix}: "
        f"host_pid={numbers['host_pid']}, role_processes=0, "
        f"client={numbers['client_threads_started']}, "
        f"parallel={numbers['parallel_threads_started']}, "
        f"dedicated={numbers['dedicated_threads_started']}, "
        f"completions={numbers['role_completions']}, active=0"
    )


def run_checked(
    arguments: Sequence[str],
    *,
    environment: Mapping[str, str] | None = None,
    timeout: float = 120.0,
) -> subprocess.CompletedProcess[str]:
    result = subprocess.run(
        list(arguments),
        env=environment,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        timeout=timeout,
    )
    if result.returncode != 0:
        command = " ".join(arguments)
        raise ThreadedRuntimeError(
            f"command failed ({result.returncode}): {command}\n"
            f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        )
    return result


def installed_prefix(install_root: Path) -> Path:
    candidates = sorted(install_root.glob("**/bin/postgres"))
    if len(candidates) != 1:
        raise ThreadedRuntimeError(
            "expected one installed postgres binary, found "
            f"{len(candidates)}"
        )
    return candidates[0].parent.parent


def available_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as listener:
        listener.bind(("127.0.0.1", 0))
        return int(listener.getsockname()[1])


def server_environment(prefix: Path) -> dict[str, str]:
    environment = os.environ.copy()
    library_paths = [prefix / "lib", prefix / "lib64"]
    paths = [str(path) for path in library_paths if path.is_dir()]
    if environment.get("LD_LIBRARY_PATH"):
        paths.append(environment["LD_LIBRARY_PATH"])
    if paths:
        environment["LD_LIBRARY_PATH"] = os.pathsep.join(paths)
    return environment


def psql_arguments(prefix: Path, socket_directory: Path, port: int) -> list[str]:
    return [
        str(prefix / "bin" / "psql"),
        "-XAt",
        "-v",
        "ON_ERROR_STOP=1",
        "-h",
        str(socket_directory),
        "-p",
        str(port),
        "postgres",
    ]


def run_query(
    base_arguments: Sequence[str],
    sql: str,
    environment: Mapping[str, str],
    *,
    timeout: float = 30.0,
) -> QueryResult:
    started = time.monotonic()
    result = subprocess.run(
        [*base_arguments, "-c", sql],
        env=environment,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        timeout=timeout,
    )
    return QueryResult(
        result.returncode,
        time.monotonic() - started,
        result.stdout.strip(),
        result.stderr.strip(),
    )


def run_concurrent_queries(
    base_arguments: Sequence[str],
    queries: Mapping[str, str],
    environment: Mapping[str, str],
    *,
    timeout: float = 15.0,
) -> dict[str, QueryResult]:
    started: dict[str, float] = {}
    processes: dict[str, subprocess.Popen[str]] = {}
    for name, sql in queries.items():
        started[name] = time.monotonic()
        processes[name] = subprocess.Popen(
            [*base_arguments, "-c", sql],
            env=environment,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )

    deadline = time.monotonic() + timeout
    pending = set(processes)
    results: dict[str, QueryResult] = {}
    try:
        while pending:
            now = time.monotonic()
            if now >= deadline:
                raise ThreadedRuntimeError(
                    "concurrent query timeout: " + ", ".join(sorted(pending))
                )
            for name in list(pending):
                process = processes[name]
                if process.poll() is None:
                    continue
                stdout, stderr = process.communicate()
                results[name] = QueryResult(
                    process.returncode,
                    now - started[name],
                    stdout.strip(),
                    stderr.strip(),
                )
                pending.remove(name)
            if pending:
                time.sleep(0.005)
    finally:
        for name in pending:
            processes[name].terminate()
        for name in pending:
            try:
                processes[name].wait(timeout=2)
            except subprocess.TimeoutExpired:
                processes[name].kill()
                processes[name].wait()
    return results


def linux_process_evidence(postmaster_pid: int) -> tuple[int | None, set[int]]:
    task_root = Path("/proc") / str(postmaster_pid) / "task"
    if not task_root.is_dir():
        return None, set()
    tasks = [path for path in task_root.iterdir() if path.name.isdigit()]
    children: set[int] = set()
    for task in tasks:
        try:
            child_text = (task / "children").read_text(encoding="ascii").strip()
        except FileNotFoundError:
            continue
        children.update(int(value) for value in child_text.split())
    return len(tasks), children


def validate_timeout_results(results: Mapping[str, QueryResult]) -> None:
    for name in ("wait_150ms", "wait_450ms", "cpu_200ms"):
        result = results[name]
        if result.returncode == 0 or "statement timeout" not in result.stderr:
            raise ThreadedRuntimeError(f"statement timeout did not fire for {name}")
        if result.elapsed_seconds > 5.0:
            raise ThreadedRuntimeError(f"statement timeout was late for {name}")
    success = results["independent_success"]
    if success.returncode != 0 or not success.stdout.endswith("42"):
        raise ThreadedRuntimeError("independent non-timeout session failed")
    if success.elapsed_seconds > 5.0:
        raise ThreadedRuntimeError("independent non-timeout session was delayed")
    if results["wait_150ms"].elapsed_seconds >= results["wait_450ms"].elapsed_seconds:
        raise ThreadedRuntimeError("per-session timeout deadlines lost their ordering")


def validate_locale_concurrency(
    base_arguments: Sequence[str], environment: Mapping[str, str]
) -> None:
    statements = []
    for _ in range(30):
        statements.extend(
            (
                "SET lc_monetary='C'",
                "SET lc_numeric='C'",
                "SET lc_time='C'",
            )
        )
    statements.append("SELECT 'locale-ok'")
    sql = "; ".join(statements) + ";"
    queries = {f"locale_{index}": sql for index in range(8)}
    for name, result in run_concurrent_queries(
        base_arguments, queries, environment, timeout=30.0
    ).items():
        if result.returncode != 0 or not result.stdout.endswith("locale-ok"):
            raise ThreadedRuntimeError(
                f"concurrent locale state failed for {name}: {result.stderr}"
            )


def validate_parallel_worker(
    base_arguments: Sequence[str], environment: Mapping[str, str]
) -> int:
    setup = run_query(
        base_arguments,
        "CREATE UNLOGGED TABLE postgamma_parallel AS "
        "SELECT value FROM generate_series(1,1000000) AS value; "
        "ALTER TABLE postgamma_parallel SET (parallel_workers=2); "
        "ANALYZE postgamma_parallel;",
        environment,
        timeout=60.0,
    )
    if setup.returncode != 0:
        raise ThreadedRuntimeError(f"parallel test setup failed: {setup.stderr}")
    result = run_query(
        base_arguments,
        "SET max_parallel_workers_per_gather=2; "
        "SET min_parallel_table_scan_size=0; "
        "SET parallel_setup_cost=0; "
        "SET parallel_tuple_cost=0; "
        "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) "
        "SELECT sum(value) FROM postgamma_parallel;",
        environment,
        timeout=60.0,
    )
    if result.returncode != 0:
        raise ThreadedRuntimeError(f"parallel query failed: {result.stderr}")
    match = re.search(r"Workers Launched:\s+([1-9][0-9]*)", result.stdout)
    if match is None:
        raise ThreadedRuntimeError(
            "parallel query did not report a launched worker\n" + result.stdout
        )
    return int(match.group(1))


def build_guc_restore_probe(
    build: Path,
    prefix: Path,
    output_directory: Path,
    environment: Mapping[str, str],
) -> Path:
    pg_config = prefix / "bin" / "pg_config"

    def config(option: str) -> str:
        return run_checked(
            [str(pg_config), option], environment=environment
        ).stdout.strip()

    # This internal probe needs generated runtime headers, which PostgreSQL's
    # native make install does not install with its server development headers.
    configuration = json.loads(
        (build / ".postgamma-configure.json").read_text(encoding="utf-8")
    )
    postgres_source = Path(configuration["source"])
    source = Path(__file__).resolve().parents[1] / "tests/runtime/guc_restore_probe.c"
    library = output_directory / "guc_restore_probe.so"
    run_checked(
        [
            *shlex.split(config("--cc")),
            *shlex.split(config("--cppflags")),
            *shlex.split(config("--cflags")),
            "-shared",
            "-fPIC",
            "-I",
            str(build / "src/include"),
            "-I",
            str(postgres_source / "src/include"),
            str(source),
            "-o",
            str(library),
        ],
        environment=environment,
    )
    return library


def validate_parallel_guc_restore(
    base_arguments: Sequence[str],
    environment: Mapping[str, str],
    probe_library: Path,
) -> None:
    library_literal = str(probe_library).replace("'", "''")
    settings_sql = (
        "SELECT setting || '|' || source FROM pg_settings "
        "WHERE name='shared_buffers'; "
    )
    result = run_query(
        base_arguments,
        f"{settings_sql}"
        "SET debug_parallel_query=on; "
        "SET max_parallel_workers_per_gather=1; "
        "SET default_text_search_config='pg_catalog.simple'; "
        f"LOAD '{library_literal}'; "
        "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) "
        "SELECT sum(value) FROM postgamma_parallel; "
        f"{settings_sql}",
        environment,
        timeout=60.0,
    )
    if result.returncode != 0:
        raise ThreadedRuntimeError(
            f"parallel GUC restoration failed: {result.stderr}"
        )
    if re.search(r"Workers Launched:\s+[1-9][0-9]*", result.stdout) is None:
        raise ThreadedRuntimeError(
            "parallel GUC restore probe did not launch a worker\n" + result.stdout
        )
    if result.stdout.splitlines().count("128|command line") != 2:
        raise ThreadedRuntimeError(
            "shared_buffers value or source was not inherited/preserved\n"
            + result.stdout
        )


def validate_external_command(
    base_arguments: Sequence[str], environment: Mapping[str, str]
) -> None:
    result = run_query(
        base_arguments,
        "COPY (SELECT 'postgamma-external-command') "
        "TO PROGRAM 'cat > /dev/null';",
        environment,
    )
    if result.returncode != 0:
        raise ThreadedRuntimeError(
            f"threaded external command failed: {result.stderr}"
        )


def validate_stack_depth(
    base_arguments: Sequence[str], environment: Mapping[str, str]
) -> None:
    configured = run_query(
        base_arguments,
        "SELECT pg_size_bytes(current_setting('max_stack_depth'));",
        environment,
    )
    if configured.returncode != 0:
        raise ThreadedRuntimeError(
            f"could not read max_stack_depth: {configured.stderr}"
        )
    try:
        configured_bytes = int(configured.stdout.splitlines()[-1])
    except (IndexError, ValueError) as error:
        raise ThreadedRuntimeError(
            f"max_stack_depth is not a byte count: {configured.stdout}"
        ) from error
    if configured_bytes < 100 * 1024 or configured_bytes >= 8 * 1024 * 1024:
        raise ThreadedRuntimeError(
            f"max_stack_depth is outside the client role stack: {configured_bytes}"
        )

    unsafe = run_query(
        base_arguments, "SET max_stack_depth='8MB';", environment
    )
    if unsafe.returncode == 0 or "max_stack_depth" not in unsafe.stderr:
        raise ThreadedRuntimeError(
            "client role accepted max_stack_depth at its configured stack size"
        )

    create = run_query(
        base_arguments,
        "CREATE FUNCTION postgamma_stack_recurse() RETURNS integer "
        "LANGUAGE SQL AS 'SELECT postgamma_stack_recurse()';",
        environment,
    )
    if create.returncode != 0:
        raise ThreadedRuntimeError(
            f"stack recursion setup failed: {create.stderr}"
        )
    try:
        recursive = run_query(
            base_arguments, "SELECT postgamma_stack_recurse();", environment
        )
        if (
            recursive.returncode == 0
            or "stack depth limit exceeded" not in recursive.stderr
        ):
            raise ThreadedRuntimeError(
                "recursive SQL did not stop at the role-thread stack limit"
            )
    finally:
        cleanup = run_query(
            base_arguments,
            "DROP FUNCTION IF EXISTS postgamma_stack_recurse();",
            environment,
        )
        if cleanup.returncode != 0:
            raise ThreadedRuntimeError(
                f"stack recursion cleanup failed: {cleanup.stderr}"
            )

    recovery = run_query(base_arguments, "SELECT 1;", environment)
    if recovery.returncode != 0 or not recovery.stdout.endswith("1"):
        raise ThreadedRuntimeError(
            "server did not recover after the stack-depth error"
        )


def validate_runtime(build: Path, make_program: str) -> None:
    with tempfile.TemporaryDirectory(prefix="postgamma-threaded-runtime-") as temporary:
        temporary_path = Path(temporary)
        install_root = temporary_path / "install"
        run_checked(
            [
                make_program,
                "-C",
                str(build),
                "install",
                f"DESTDIR={install_root}",
            ],
            timeout=180.0,
        )
        prefix = installed_prefix(install_root)
        environment = server_environment(prefix)
        guc_restore_probe = build_guc_restore_probe(
            build, prefix, temporary_path, environment
        )
        data_directory = temporary_path / "data"
        log_path = temporary_path / "postmaster.log"
        socket_directory = temporary_path / "socket"
        socket_directory.mkdir()
        port = available_port()
        initdb = prefix / "bin" / "initdb"
        pg_ctl = prefix / "bin" / "pg_ctl"
        run_checked(
            [
                str(initdb),
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
            f"-F -c listen_addresses='' "
            f"-c unix_socket_directories={socket_directory} -p {port} "
            "-c max_connections=30 -c max_worker_processes=8 "
            "-c shared_buffers=128 "
            "-c max_parallel_workers=4 -c log_min_messages=log"
        )
        server_running = False
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
            native_threads, process_children = linux_process_evidence(postmaster_pid)
            if native_threads is not None and native_threads < 2:
                raise ThreadedRuntimeError("native role threads were not observed")
            if process_children:
                raise ThreadedRuntimeError(
                    "PostgreSQL role process children were observed: "
                    + ", ".join(str(pid) for pid in sorted(process_children))
                )

            base_arguments = psql_arguments(prefix, socket_directory, port)
            pid_result = run_query(
                base_arguments, "SELECT pg_backend_pid();", environment
            )
            if pid_result.returncode != 0:
                raise ThreadedRuntimeError(
                    f"could not query compatibility PID: {pid_result.stderr}"
                )
            compatibility_pid = int(pid_result.stdout.splitlines()[-1])
            if compatibility_pid < COMPAT_PID_FIRST:
                raise ThreadedRuntimeError(
                    "client backend exposed an operating-system PID"
                )

            timeout_results = run_concurrent_queries(
                base_arguments,
                {
                    "wait_150ms": (
                        "SET statement_timeout='150ms'; SELECT pg_sleep(10);"
                    ),
                    "wait_450ms": (
                        "SET statement_timeout='450ms'; SELECT pg_sleep(10);"
                    ),
                    "independent_success": (
                        "SET statement_timeout=0; "
                        "SELECT pg_sleep(0.25); SELECT 42;"
                    ),
                    "cpu_200ms": (
                        "SET max_parallel_workers_per_gather=0; "
                        "SET statement_timeout='200ms'; "
                        "SELECT count(*) FROM generate_series(1,1000000000);"
                    ),
                },
                environment,
            )
            validate_timeout_results(timeout_results)
            validate_locale_concurrency(base_arguments, environment)
            launched_parallel_workers = validate_parallel_worker(
                base_arguments, environment
            )
            validate_parallel_guc_restore(
                base_arguments, environment, guc_restore_probe
            )
            validate_external_command(base_arguments, environment)
            validate_stack_depth(base_arguments, environment)

            _, process_children = linux_process_evidence(postmaster_pid)
            if process_children:
                raise ThreadedRuntimeError(
                    "PostgreSQL role process children were observed after workload: "
                    + ", ".join(str(pid) for pid in sorted(process_children))
                )
            run_checked(
                [
                    str(pg_ctl),
                    "-D",
                    str(data_directory),
                    "-m",
                    "fast",
                    "-w",
                    "stop",
                ],
                environment=environment,
            )
            server_running = False
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

        numbers = validate_telemetry_log(log_path)
        native_text = "not-available" if native_threads is None else str(native_threads)
        process_text = "not-available" if native_threads is None else "0"
        print(
            "threaded runtime: "
            f"compat_pid={compatibility_pid}, native_threads={native_text}, "
            f"os_role_processes={process_text}, "
            f"parallel_workers={launched_parallel_workers}, parallel_guc_restore=ok"
        )
        print_telemetry(numbers, prefix="threaded telemetry")


def main() -> int:
    parser = argparse.ArgumentParser()
    input_group = parser.add_mutually_exclusive_group(required=True)
    input_group.add_argument("--build", type=Path)
    input_group.add_argument("--telemetry-log", type=Path)
    parser.add_argument("--make", default="make")
    args = parser.parse_args()
    try:
        if args.build is not None:
            validate_runtime(args.build.resolve(), args.make)
        else:
            numbers = validate_telemetry_log(args.telemetry_log.resolve())
            print_telemetry(numbers, prefix="threaded regression telemetry")
    except (OSError, subprocess.SubprocessError, ThreadedRuntimeError) as error:
        print(f"threaded runtime check failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
