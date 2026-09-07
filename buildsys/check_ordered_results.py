#!/usr/bin/env python3
"""Prove ordered results ordered results against an external PG19 Oracle."""

from __future__ import annotations

import argparse
import json
import os
import re
import tempfile
from pathlib import Path
from typing import Any

from check_embedded_lifecycle import (
    LifecycleCheckError,
    input_identity,
    installed_prefix,
    run_checked,
    runtime_environment,
    write_json,
)
from check_embedded_public_api import PublicApiCheckError, audit_trace


CASE_MARKER = "POSTGAMMA_API_CASE"
REFERENCE_MARKER = "POSTGAMMA_API_REFERENCE"
CANDIDATE_MARKER = "POSTGAMMA_API_ORDERED_RESULTS"
REFERENCE_KIND = "postgamma.ordered-results-reference"
CANDIDATE_KIND = "postgamma.ordered-results-candidate"
AGGREGATE_KIND = "postgamma.ordered-results"
EXPECTED_CASES = (
    ("extended", "0", "tuples"),
    ("script", "0", "tuples"),
    ("script", "1", "command"),
    ("script", "2", "tuples"),
    ("script_error", "0", "tuples"),
    ("script_error", "1", "error"),
    ("reuse_after_error", "0", "tuples"),
    ("binary", "0", "tuples"),
    ("prepared_inferred", "0", "description"),
    ("prepared_execute", "0", "tuples"),
    ("prepared_execute", "1", "tuples"),
    ("prepared_explicit", "0", "description"),
    ("prepared_explicit_execute", "0", "tuples"),
    ("transaction", "0", "status"),
    ("transaction", "1", "status"),
    ("cancel", "0", "error"),
    ("reuse_after_cancel", "0", "tuples"),
)
COMMON_MARKER_VALUES = {
    "postgres": "19",
    "ordered_results": "6",
    "prepared_executions": "3",
    "text_binary": "true",
    "reuse_after_error": "true",
    "cancel": "true",
    "reuse_after_cancel": "true",
    "phase": "closed",
}
CANDIDATE_ONLY_MARKER_VALUES = {
    "connections": "2",
    "sql_length": "true",
    "ownership": "true",
    "transaction_pin": "true",
    "advisory_pin": "true",
    "prepared_capability": "true",
}


class OrderedResultsCheckError(RuntimeError):
    """The ordered-results semantic or isolation contract was violated."""


def fields(line: str) -> dict[str, str]:
    return dict(re.findall(r"([a-z_]+)=([^\s]*)", line))


def parse_cases(output: str) -> list[str]:
    lines = [
        line.strip()
        for line in output.splitlines()
        if line.startswith(f"{CASE_MARKER} ")
    ]
    observed = tuple(
        (entry.get("name", ""), entry.get("sequence", ""), entry.get("kind", ""))
        for entry in (fields(line) for line in lines)
    )
    if observed != EXPECTED_CASES:
        raise OrderedResultsCheckError(
            f"semantic case sequence mismatch: expected={EXPECTED_CASES}, observed={observed}"
        )
    return lines


def parse_marker(output: str, marker: str, *, candidate: bool) -> dict[str, str]:
    lines = [line for line in output.splitlines() if line.startswith(f"{marker} ")]
    if len(lines) != 1:
        raise OrderedResultsCheckError(
            f"expected one {marker} line, found {len(lines)}"
        )
    values = fields(lines[0])
    required = dict(COMMON_MARKER_VALUES)
    if candidate:
        required.update(CANDIDATE_ONLY_MARKER_VALUES)
    for name, expected in required.items():
        if values.get(name) != expected:
            raise OrderedResultsCheckError(
                f"{marker} field {name} is not {expected}"
            )
    return values


def parse_runtime(stderr: str) -> dict[str, str | int]:
    lines = [line for line in stderr.splitlines() if "POSTGAMMA_RUNTIME" in line]
    if len(lines) != 1:
        raise OrderedResultsCheckError(
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
            raise OrderedResultsCheckError(
                f"candidate runtime field {name} is not {expected}"
            )
    numeric_names = (
        "host_pid",
        "client_threads_started",
        "pooled_worker_threads",
        "client_quantums",
        "quantum_yields",
        "carrier_migrations",
        "pinned_sessions_peak",
        "execution_token_budget",
        "execution_tokens_peak",
    )
    try:
        numeric = {name: int(values.get(name, "")) for name in numeric_names}
    except ValueError as exc:
        raise OrderedResultsCheckError("candidate runtime counters are invalid") from exc
    if numeric["host_pid"] <= 0:
        raise OrderedResultsCheckError("candidate host process identity is invalid")
    if numeric["client_threads_started"] < 2:
        raise OrderedResultsCheckError("candidate did not run both public connections")
    if numeric["pooled_worker_threads"] != 4:
        raise OrderedResultsCheckError("candidate did not use the four-worker profile")
    if (
        numeric["client_quantums"] <= numeric["client_threads_started"]
        or numeric["quantum_yields"] == 0
        or numeric["carrier_migrations"] == 0
    ):
        raise OrderedResultsCheckError("candidate did not exercise resumable quantums")
    if numeric["pinned_sessions_peak"] == 0:
        raise OrderedResultsCheckError("candidate did not observe a pinned session")
    if (
        numeric["execution_token_budget"] != 4
        or numeric["execution_tokens_peak"] <= 0
        or numeric["execution_tokens_peak"] > 4
    ):
        raise OrderedResultsCheckError("candidate execution-token budget is invalid")
    if "FATAL:" in stderr or "PANIC:" in stderr:
        raise OrderedResultsCheckError("candidate emitted a fatal backend error")
    return {**values, **numeric}


def load_report(path: Path, expected_kind: str) -> dict[str, Any]:
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise OrderedResultsCheckError(f"cannot read evidence {path}: {exc}") from exc
    if (
        not isinstance(document, dict)
        or document.get("schema_version") != 1
        or document.get("kind") != expected_kind
        or document.get("status") != "pass"
    ):
        raise OrderedResultsCheckError(f"invalid evidence document: {path}")
    return document


def write_text(path: Path, content: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content, encoding="utf-8")


def prepend_library_path(environment: dict[str, str], path: Path) -> None:
    values = [str(path)]
    if environment.get("LD_LIBRARY_PATH"):
        values.append(environment["LD_LIBRARY_PATH"])
    environment["LD_LIBRARY_PATH"] = os.pathsep.join(values)


def run_reference(args: argparse.Namespace) -> None:
    generated = (args.stdout, args.stderr, args.server_log, args.output)
    for path in generated:
        path.resolve().parent.mkdir(parents=True, exist_ok=True)
        path.resolve().unlink(missing_ok=True)
    reference_build = args.postgres_build.resolve(strict=True)
    driver = args.driver.resolve(strict=True)
    inputs = [path.resolve(strict=True) for path in args.input]
    work_root = args.work_root.resolve()
    work_root.mkdir(parents=True, exist_ok=True)
    reference_pid = 0
    completed = None
    # Unix socket paths must fit sockaddr_un even in deeply nested workspaces.
    with (
        tempfile.TemporaryDirectory(prefix="api-reference-", dir=work_root) as temp,
        tempfile.TemporaryDirectory(prefix="pgm-api-socket-") as socket_temp,
    ):
        temporary = Path(temp)
        install_root = temporary / "install"
        run_checked(
            [
                args.make,
                "-C",
                str(reference_build),
                f"-j{args.jobs}",
                "install",
                f"DESTDIR={install_root}",
            ],
            timeout=300.0,
        )
        prefix = installed_prefix(install_root)
        environment = runtime_environment(prefix)
        environment["LC_ALL"] = "C"
        data_directory = temporary / "data"
        socket_directory = Path(socket_temp)
        # Keep startup diagnostics after the temporary cluster is removed.
        log_path = args.server_log.resolve()
        run_checked(
            [
                str(prefix / "bin" / "initdb"),
                "--pgdata",
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
            + "\nlisten_addresses = ''\n"
            + f"unix_socket_directories = '{socket_directory}'\n"
            + "port = 65432\n"
            + "fsync = off\n",
            encoding="utf-8",
        )
        started = False
        try:
            run_checked(
                [
                    str(prefix / "bin" / "pg_ctl"),
                    "-D",
                    str(data_directory),
                    "-l",
                    str(log_path),
                    "-w",
                    "start",
                ],
                environment=environment,
                timeout=60.0,
            )
            started = True
            reference_pid = int(
                (data_directory / "postmaster.pid")
                .read_text(encoding="utf-8")
                .splitlines()[0]
            )
            completed = run_checked(
                [str(driver), str(socket_directory), "65432"],
                environment=environment,
                timeout=60.0,
                check=False,
            )
        finally:
            if started:
                run_checked(
                    [
                        str(prefix / "bin" / "pg_ctl"),
                        "-D",
                        str(data_directory),
                        "-m",
                        "fast",
                        "-w",
                        "stop",
                    ],
                    environment=environment,
                    timeout=60.0,
                    check=False,
                )
    if completed is None:
        raise OrderedResultsCheckError("reference driver did not run")
    write_text(args.stdout.resolve(), completed.stdout)
    write_text(args.stderr.resolve(), completed.stderr)
    if completed.returncode != 0:
        raise OrderedResultsCheckError(
            f"reference driver failed ({completed.returncode})\n"
            f"stdout:\n{completed.stdout}\nstderr:\n{completed.stderr}"
        )
    cases = parse_cases(completed.stdout)
    marker = parse_marker(completed.stdout, REFERENCE_MARKER, candidate=False)
    if reference_pid <= 0:
        raise OrderedResultsCheckError("reference postmaster identity is invalid")
    write_json(
        args.output.resolve(),
        {
            "schema_version": 1,
            "kind": REFERENCE_KIND,
            "status": "pass",
            "postgresql_major": 19,
            "reference_process_id": reference_pid,
            "driver": str(driver),
            "marker": marker,
            "semantic_cases": cases,
            "scope": {
                "ordinary_socket_libpq": True,
                "external_process_allowed": True,
                "network_socket_allowed": True,
                "inside_embedded_trace": False,
            },
            "inputs": input_identity(inputs),
        },
    )


def run_candidate(args: argparse.Namespace) -> None:
    generated = (args.stdout, args.stderr, args.trace, args.output)
    for path in generated:
        path.resolve().parent.mkdir(parents=True, exist_ok=True)
        path.resolve().unlink(missing_ok=True)
    postgres_build = args.postgres_build.resolve(strict=True)
    library = args.library.resolve(strict=True)
    driver = args.driver.resolve(strict=True)
    reference_path = args.reference.resolve(strict=True)
    reference = load_report(reference_path, REFERENCE_KIND)
    inputs = [path.resolve(strict=True) for path in args.input]
    work_root = args.work_root.resolve()
    work_root.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="api-candidate-", dir=work_root) as temp:
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
        prepend_library_path(environment, library.parent)
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
            command, environment=environment, timeout=90.0, check=False
        )
    write_text(args.stdout.resolve(), completed.stdout)
    write_text(args.stderr.resolve(), completed.stderr)
    if completed.returncode != 0:
        raise OrderedResultsCheckError(
            f"candidate driver failed ({completed.returncode})\n"
            f"stdout:\n{completed.stdout}\nstderr:\n{completed.stderr}"
        )
    cases = parse_cases(completed.stdout)
    marker = parse_marker(completed.stdout, CANDIDATE_MARKER, candidate=True)
    runtime = parse_runtime(completed.stderr)
    if cases != reference.get("semantic_cases"):
        raise OrderedResultsCheckError(
            "embedded semantic transcript differs from the socket-libpq Oracle"
        )
    trace_text = args.trace.resolve().read_text(encoding="utf-8")
    host_safety = audit_trace(trace_text, driver)
    reference_pid = reference.get("reference_process_id")
    candidate_pid = runtime["host_pid"]
    if not isinstance(reference_pid, int) or reference_pid <= 0:
        raise OrderedResultsCheckError("reference process identity is invalid")
    if reference_pid == candidate_pid:
        raise OrderedResultsCheckError("reference and candidate process scopes overlap")
    write_json(
        args.output.resolve(),
        {
            "schema_version": 1,
            "kind": CANDIDATE_KIND,
            "status": "pass",
            "postgresql_major": 19,
            "candidate_process_id": candidate_pid,
            "reference_process_id": reference_pid,
            "driver": str(driver),
            "library": str(library),
            "marker": marker,
            "semantic_cases": cases,
            "semantic_match": True,
            "runtime": runtime,
            "host_safety": host_safety,
            "trace_scope": {
                "candidate_only": True,
                "reference_processes": 0,
                "reference_sockets": 0,
            },
            "inputs": input_identity([*inputs, reference_path]),
        },
    )


def run_aggregate(args: argparse.Namespace) -> None:
    reference_path = args.reference.resolve(strict=True)
    candidate_path = args.candidate.resolve(strict=True)
    reference = load_report(reference_path, REFERENCE_KIND)
    candidate = load_report(candidate_path, CANDIDATE_KIND)
    if reference.get("semantic_cases") != candidate.get("semantic_cases"):
        raise OrderedResultsCheckError("aggregate semantic evidence does not match")
    if reference.get("reference_process_id") != candidate.get("reference_process_id"):
        raise OrderedResultsCheckError("aggregate reference identity does not match")
    if reference.get("reference_process_id") == candidate.get("candidate_process_id"):
        raise OrderedResultsCheckError("aggregate process scopes overlap")
    host_safety = candidate.get("host_safety")
    if not isinstance(host_safety, dict) or any(
        host_safety.get(name) != 0
        for name in (
            "process_creation_calls",
            "host_signal_delivery_calls",
            "network_endpoint_calls",
            "process_global_state_calls",
        )
    ):
        raise OrderedResultsCheckError("aggregate candidate host-safety proof is invalid")
    write_json(
        args.output.resolve(),
        {
            "schema_version": 1,
            "kind": AGGREGATE_KIND,
            "status": "pass",
            "postgresql_major": 19,
            "semantic_case_count": len(EXPECTED_CASES),
            "ordered_result_count": 6,
            "prepared_execution_count": 3,
            "socket_libpq_matches_embedded_public_api": True,
            "connection_reusable_after_error": True,
            "connection_reusable_after_cancel": True,
            "text_and_binary_parameters_and_results": True,
            "inferred_and_explicit_parameter_oids": True,
            "transaction_and_pin_status": True,
            "reference_process_id": reference["reference_process_id"],
            "candidate_process_id": candidate["candidate_process_id"],
            "candidate_host_safety": host_safety,
            "inputs": input_identity([reference_path, candidate_path]),
        },
    )


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser()
    subparsers = result.add_subparsers(dest="phase", required=True)

    reference = subparsers.add_parser("reference")
    reference.add_argument("--make", default="make")
    reference.add_argument("--jobs", type=int, default=1)
    reference.add_argument("--postgres-build", required=True, type=Path)
    reference.add_argument("--driver", required=True, type=Path)
    reference.add_argument("--work-root", required=True, type=Path)
    reference.add_argument("--stdout", required=True, type=Path)
    reference.add_argument("--stderr", required=True, type=Path)
    reference.add_argument("--server-log", required=True, type=Path)
    reference.add_argument("--input", action="append", default=[], type=Path)
    reference.add_argument("--output", required=True, type=Path)

    candidate = subparsers.add_parser("candidate")
    candidate.add_argument("--make", default="make")
    candidate.add_argument("--jobs", type=int, default=1)
    candidate.add_argument("--postgres-build", required=True, type=Path)
    candidate.add_argument("--library", required=True, type=Path)
    candidate.add_argument("--driver", required=True, type=Path)
    candidate.add_argument("--reference", required=True, type=Path)
    candidate.add_argument("--work-root", required=True, type=Path)
    candidate.add_argument("--stdout", required=True, type=Path)
    candidate.add_argument("--stderr", required=True, type=Path)
    candidate.add_argument("--trace", required=True, type=Path)
    candidate.add_argument("--strace", default="strace")
    candidate.add_argument("--input", action="append", default=[], type=Path)
    candidate.add_argument("--output", required=True, type=Path)

    aggregate = subparsers.add_parser("aggregate")
    aggregate.add_argument("--reference", required=True, type=Path)
    aggregate.add_argument("--candidate", required=True, type=Path)
    aggregate.add_argument("--output", required=True, type=Path)
    return result


def main() -> int:
    args = parser().parse_args()
    try:
        if args.phase == "reference":
            run_reference(args)
            print("reference SQL oracle: pass (ordinary socket libpq on PG19)")
        elif args.phase == "candidate":
            run_candidate(args)
            print("embedded SQL conformance: pass (public API == PG19 oracle)")
        else:
            run_aggregate(args)
            print("ordered-results evidence: pass")
    except (
        OSError,
        ValueError,
        LifecycleCheckError,
        PublicApiCheckError,
        OrderedResultsCheckError,
    ) as exc:
        parser().error(str(exc))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
