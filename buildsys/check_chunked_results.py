#!/usr/bin/env python3
"""Prove bounded tuple streaming against PostgreSQL 19 libpq."""

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
from check_embedded_public_api import audit_trace


CASE_MARKER = "POSTGAMMA_STREAMING_CASE"
REFERENCE_MARKER = "POSTGAMMA_STREAMING_REFERENCE"
CANDIDATE_MARKER = "POSTGAMMA_STREAMING_CHUNKED"
REFERENCE_KIND = "postgamma.chunked-results-reference"
CANDIDATE_KIND = "postgamma.chunked-results-candidate"
AGGREGATE_KIND = "postgamma.chunked-results"
EXPECTED_CASES = (
    ("basic", "0", "tuples_chunk", "3", "1", "0"),
    ("basic", "1", "tuples_chunk", "3", "1", "0"),
    ("basic", "2", "tuples_chunk", "3", "1", "0"),
    ("basic", "3", "tuples_chunk", "1", "1", "0"),
    ("basic", "4", "tuples", "0", "1", "0"),
    ("binary", "0", "tuples_chunk", "2", "1", "1"),
    ("binary", "1", "tuples_chunk", "2", "1", "1"),
    ("binary", "2", "tuples_chunk", "1", "1", "1"),
    ("binary", "3", "tuples", "0", "1", "1"),
    ("script", "0", "tuples_chunk", "2", "1", "0"),
    ("script", "1", "tuples_chunk", "2", "1", "0"),
    ("script", "2", "tuples", "0", "1", "0"),
    ("script", "3", "command", "0", "0", "0"),
    ("script", "4", "tuples_chunk", "2", "1", "0"),
    ("script", "5", "tuples_chunk", "1", "1", "0"),
    ("script", "6", "tuples", "0", "1", "0"),
    ("late_error", "0", "tuples_chunk", "3", "1", "0"),
    ("late_error", "1", "tuples_chunk", "3", "1", "0"),
    ("late_error", "2", "error", "0", "0", "0"),
    ("reuse_after_late_error", "0", "tuples", "1", "1", "0"),
)
COMMON_MARKER_VALUES = {
    "postgres": "19",
    "chunked_sequences": "5",
    "common_streamed_rows": "28",
    "common_terminal_results": "6",
    "text_binary": "true",
    "script": "true",
    "late_error": "true",
    "connection_reuse": "true",
    "phase": "closed",
}
CANDIDATE_ONLY_MARKER_VALUES = {
    "observed_terminal_results": "7",
    "wait_nonconsuming": "true",
    "ownership": "true",
    "limit_sqlstate": "54000",
    "maximum_value_limit": "true",
    "result_buffer_limit": "true",
    "slow_rows": "10000000",
    "slow_chunks": "39063",
    "slow_terminal": "true",
    "stalled_consumer": "true",
    "thread_growth": "0",
    "rss_allowance_bytes": "67108864",
    "partial_retirement": "true",
    "chunked_capability": "true",
}


class ChunkedResultsCheckError(RuntimeError):
    """The streaming, memory, or isolation contract was violated."""


def fields(line: str) -> dict[str, str]:
    return dict(re.findall(r"([a-z_]+)=([^\s]*)", line))


def parse_cases(output: str) -> list[str]:
    lines = [
        line.strip()
        for line in output.splitlines()
        if line.startswith(f"{CASE_MARKER} ")
    ]
    observed = tuple(
        (
            entry.get("name", ""),
            entry.get("sequence", ""),
            entry.get("kind", ""),
            entry.get("rows", ""),
            entry.get("columns", ""),
            entry.get("format", ""),
        )
        for entry in (fields(line) for line in lines)
    )
    if observed != EXPECTED_CASES:
        raise ChunkedResultsCheckError(
            f"streaming case sequence mismatch: expected={EXPECTED_CASES}, "
            f"observed={observed}"
        )
    return lines


def parse_marker(output: str, marker: str, *, candidate: bool) -> dict[str, str]:
    lines = [line for line in output.splitlines() if line.startswith(f"{marker} ")]
    if len(lines) != 1:
        raise ChunkedResultsCheckError(
            f"expected one {marker} line, found {len(lines)}"
        )
    values = fields(lines[0])
    required = dict(COMMON_MARKER_VALUES)
    if candidate:
        required.update(CANDIDATE_ONLY_MARKER_VALUES)
    for name, expected in required.items():
        if values.get(name) != expected:
            raise ChunkedResultsCheckError(f"{marker} field {name} is not {expected}")
    if candidate:
        try:
            rss_delta = int(values.get("rss_delta_bytes", ""))
            rss_allowance = int(values["rss_allowance_bytes"])
        except ValueError as exc:
            raise ChunkedResultsCheckError("candidate RSS counters are invalid") from exc
        if rss_delta < 0 or rss_delta > rss_allowance:
            raise ChunkedResultsCheckError(
                f"candidate RSS delta {rss_delta} exceeds allowance {rss_allowance}"
            )
    return values


def parse_runtime(stderr: str) -> dict[str, str | int]:
    lines = [line for line in stderr.splitlines() if "POSTGAMMA_RUNTIME" in line]
    if len(lines) != 1:
        raise ChunkedResultsCheckError(
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
            raise ChunkedResultsCheckError(
                f"candidate runtime field {name} is not {expected}"
            )
    numeric_names = (
        "host_pid",
        "client_threads_started",
        "pooled_worker_threads",
        "client_quantums",
        "quantum_yields",
        "execution_token_budget",
        "execution_tokens_peak",
    )
    try:
        numeric = {name: int(values.get(name, "")) for name in numeric_names}
    except ValueError as exc:
        raise ChunkedResultsCheckError("candidate runtime counters are invalid") from exc
    if numeric["host_pid"] <= 0:
        raise ChunkedResultsCheckError("candidate host process identity is invalid")
    if numeric["client_threads_started"] < 1:
        raise ChunkedResultsCheckError("candidate did not start a client session")
    if numeric["pooled_worker_threads"] != 4:
        raise ChunkedResultsCheckError("candidate did not use the four-worker profile")
    if numeric["client_quantums"] <= 0 or numeric["quantum_yields"] <= 0:
        raise ChunkedResultsCheckError("candidate did not exercise resumable quantums")
    if (
        numeric["execution_token_budget"] != 4
        or numeric["execution_tokens_peak"] <= 0
        or numeric["execution_tokens_peak"] > 4
    ):
        raise ChunkedResultsCheckError("candidate execution-token budget is invalid")
    if "FATAL:" in stderr or "PANIC:" in stderr:
        raise ChunkedResultsCheckError("candidate emitted a fatal backend error")
    return {**values, **numeric}


def load_report(path: Path, expected_kind: str) -> dict[str, Any]:
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise ChunkedResultsCheckError(f"cannot read evidence {path}: {exc}") from exc
    if (
        not isinstance(document, dict)
        or document.get("schema_version") != 1
        or document.get("kind") != expected_kind
        or document.get("status") != "pass"
    ):
        raise ChunkedResultsCheckError(f"invalid evidence document: {path}")
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
        tempfile.TemporaryDirectory(
            prefix="chunked_results-reference-", dir=work_root
        ) as temp,
        tempfile.TemporaryDirectory(prefix="pgm-chunk-socket-") as socket_temp,
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
            + "port = 65433\n"
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
                [str(driver), str(socket_directory), "65433"],
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
        raise ChunkedResultsCheckError("reference driver did not run")
    write_text(args.stdout.resolve(), completed.stdout)
    write_text(args.stderr.resolve(), completed.stderr)
    if completed.returncode != 0:
        raise ChunkedResultsCheckError(
            f"reference driver failed ({completed.returncode})\n"
            f"stdout:\n{completed.stdout}\nstderr:\n{completed.stderr}"
        )
    cases = parse_cases(completed.stdout)
    marker = parse_marker(completed.stdout, REFERENCE_MARKER, candidate=False)
    if reference_pid <= 0:
        raise ChunkedResultsCheckError("reference postmaster identity is invalid")
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
    with tempfile.TemporaryDirectory(prefix="chunked_results-candidate-", dir=work_root) as temp:
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
        completed = run_checked(
            [
                str(driver),
                str(temporary / "data"),
                str(prefix / "bin" / "postgres"),
                str(prefix),
                "create",
            ],
            environment=environment,
            timeout=360.0,
            check=False,
        )
        trace_completed = run_checked(
            [
                args.strace,
                "-f",
                "-qq",
                "-o",
                str(args.trace.resolve()),
                "-e",
                "trace=process,signal,network,chdir,fchdir,umask,setitimer",
                str(driver),
                str(temporary / "trace-data"),
                str(prefix / "bin" / "postgres"),
                str(prefix),
                "create",
                "trace",
            ],
            environment=environment,
            timeout=120.0,
            check=False,
        )
    write_text(args.stdout.resolve(), completed.stdout)
    write_text(args.stderr.resolve(), completed.stderr)
    if completed.returncode != 0:
        raise ChunkedResultsCheckError(
            f"candidate driver failed ({completed.returncode})\n"
            f"stdout:\n{completed.stdout}\nstderr:\n{completed.stderr}"
        )
    if trace_completed.returncode != 0:
        raise ChunkedResultsCheckError(
            f"candidate trace driver failed ({trace_completed.returncode})\n"
            f"stdout:\n{trace_completed.stdout}\n"
            f"stderr:\n{trace_completed.stderr}"
        )
    cases = parse_cases(completed.stdout)
    marker = parse_marker(completed.stdout, CANDIDATE_MARKER, candidate=True)
    runtime = parse_runtime(completed.stderr)
    trace_runtime = parse_runtime(trace_completed.stderr)
    if cases != reference.get("semantic_cases"):
        raise ChunkedResultsCheckError(
            "embedded chunk transcript differs from the socket-libpq Oracle"
        )
    trace_text = args.trace.resolve().read_text(encoding="utf-8")
    host_safety = audit_trace(trace_text, driver)
    reference_pid = reference.get("reference_process_id")
    candidate_pid = runtime["host_pid"]
    trace_candidate_pid = trace_runtime["host_pid"]
    if not isinstance(reference_pid, int) or reference_pid <= 0:
        raise ChunkedResultsCheckError("reference process identity is invalid")
    if reference_pid == candidate_pid:
        raise ChunkedResultsCheckError("reference and candidate process scopes overlap")
    if reference_pid == trace_candidate_pid or candidate_pid == trace_candidate_pid:
        raise ChunkedResultsCheckError("candidate trace process scopes overlap")
    write_json(
        args.output.resolve(),
        {
            "schema_version": 1,
            "kind": CANDIDATE_KIND,
            "status": "pass",
            "postgresql_major": 19,
            "candidate_process_id": candidate_pid,
            "trace_candidate_process_id": trace_candidate_pid,
            "reference_process_id": reference_pid,
            "driver": str(driver),
            "library": str(library),
            "marker": marker,
            "semantic_cases": cases,
            "semantic_match": True,
            "runtime": runtime,
            "trace_runtime": trace_runtime,
            "host_safety": host_safety,
            "trace_scope": {
                "candidate_only": True,
                "candidate_process_id": trace_candidate_pid,
                "reference_processes": 0,
                "reference_sockets": 0,
            },
            "inputs": input_identity([*inputs, library, reference_path]),
        },
    )


def run_aggregate(args: argparse.Namespace) -> None:
    reference_path = args.reference.resolve(strict=True)
    candidate_path = args.candidate.resolve(strict=True)
    reference = load_report(reference_path, REFERENCE_KIND)
    candidate = load_report(candidate_path, CANDIDATE_KIND)
    if reference.get("semantic_cases") != candidate.get("semantic_cases"):
        raise ChunkedResultsCheckError("aggregate streaming evidence does not match")
    if reference.get("reference_process_id") != candidate.get("reference_process_id"):
        raise ChunkedResultsCheckError("aggregate reference identity does not match")
    if reference.get("reference_process_id") == candidate.get("candidate_process_id"):
        raise ChunkedResultsCheckError("aggregate process scopes overlap")
    if reference.get("reference_process_id") == candidate.get(
        "trace_candidate_process_id"
    ) or candidate.get("candidate_process_id") == candidate.get(
        "trace_candidate_process_id"
    ):
        raise ChunkedResultsCheckError("aggregate trace process scopes overlap")
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
        raise ChunkedResultsCheckError("aggregate candidate host-safety proof is invalid")
    marker = candidate.get("marker")
    if not isinstance(marker, dict):
        raise ChunkedResultsCheckError("aggregate candidate marker is invalid")
    write_json(
        args.output.resolve(),
        {
            "schema_version": 1,
            "kind": AGGREGATE_KIND,
            "status": "pass",
            "postgresql_major": 19,
            "semantic_case_count": len(EXPECTED_CASES),
            "streamed_reference_rows": 28,
            "large_stream_rows": 10_000_000,
            "large_stream_chunks": 39_063,
            "rss_delta_bytes": int(marker["rss_delta_bytes"]),
            "rss_allowance_bytes": int(marker["rss_allowance_bytes"]),
            "bounded_result_and_value_limits": True,
            "slow_consumer_thread_growth": 0,
            "terminal_result_observed_for_every_sequence": True,
            "partial_request_retirement": True,
            "connection_reusable_after_late_and_limit_errors": True,
            "socket_libpq_matches_embedded_public_api": True,
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
        elif args.phase == "candidate":
            run_candidate(args)
        else:
            run_aggregate(args)
    except (ChunkedResultsCheckError, LifecycleCheckError, OSError, ValueError) as exc:
        raise SystemExit(f"chunked-results check failed: {exc}") from exc
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
