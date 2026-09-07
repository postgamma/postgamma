#!/usr/bin/env python3
"""Execute and record a real PG19 query over the private memory transport."""

from __future__ import annotations

import argparse
import os
import re
import tempfile
from collections import Counter
from pathlib import Path

from check_embedded_lifecycle import (
    LifecycleCheckError,
    input_identity,
    installed_prefix,
    load_resource_budget,
    parse_backend_stack_telemetry,
    run_checked,
    runtime_environment,
    sha256,
    validate_backend_stack_budget,
    validate_supervisor_stack_budget,
    write_json,
)


MARKER = "POSTGAMMA_KERNEL_QUERY"
EXPECTED_GENERATION = 620001
REQUIRED_MARKER_VALUES = {
    "executor": "pooled",
    "connected": "true",
    "select_one": "true",
    "error_sqlstate": "22012",
    "error_recovery": "true",
    "extended_params": "true",
    "dynamic_result": "true",
    "binary_result": "true",
    "cancellation": "true",
    "settings_precedence": "true",
    "safety_policy": "true",
    "transport": "memory",
    "network_calls": "0",
    "active_transports_after_close": "0",
    "endpoint_references_after_close": "0",
    "phase": "closed",
    "state": "closed",
}
FORBIDDEN_SIGNAL_CALLS = frozenset(
    {"kill", "pidfd_send_signal", "rt_sigqueueinfo", "tgkill", "tkill"}
)
FORBIDDEN_NETWORK_CALLS = frozenset(
    {
        "accept",
        "accept4",
        "bind",
        "connect",
        "getpeername",
        "getsockname",
        "getsockopt",
        "listen",
        "recvfrom",
        "recvmsg",
        "sendmsg",
        "sendto",
        "setsockopt",
        "shutdown",
        "socket",
        "socketpair",
    }
)
FORBIDDEN_HOST_STATE_CALLS = frozenset(
    {"chdir", "fchdir", "setitimer", "umask"}
)
CALL_PATTERN = re.compile(r"(?:^|\s)([a-zA-Z_][a-zA-Z0-9_]*)\(")


class QueryCheckError(RuntimeError):
    """The real embedded query did not meet its evidence contract."""


def validate_policy_rejection(
    returncode: int, stdout: str, stderr: str
) -> dict[str, object]:
    expected = (
        'unsafe setting "restart_after_crash" is not allowed in embedded mode'
    )
    if returncode == 0:
        raise QueryCheckError("unsafe embedded configuration was accepted")
    if expected not in stderr or "configuration file" not in stderr:
        raise QueryCheckError(
            "unsafe embedded configuration did not report its setting origin"
        )
    return {
        "status": "pass",
        "setting": "restart_after_crash",
        "requested_value": "on",
        "required_value": "off",
        "origin": "configuration file",
        "driver_returncode": returncode,
        "stdout_bytes": len(stdout.encode("utf-8")),
        "stderr_bytes": len(stderr.encode("utf-8")),
    }


def parse_positive(values: dict[str, str], name: str) -> int:
    try:
        value = int(values.get(name, ""))
    except ValueError as exc:
        raise QueryCheckError(f"query marker {name} is not an integer") from exc
    if value <= 0:
        raise QueryCheckError(f"query marker {name} is not positive")
    return value


def parse_marker(output: str) -> dict[str, str]:
    lines = [line for line in output.splitlines() if MARKER in line]
    if len(lines) != 1:
        raise QueryCheckError(
            f"expected one {MARKER} line, found {len(lines)}"
        )
    values = dict(re.findall(r"([a-z_]+)=([^\s]+)", lines[0]))
    try:
        generation = int(values.get("generation", ""))
    except ValueError as exc:
        raise QueryCheckError("query generation is not an integer") from exc
    if generation != EXPECTED_GENERATION:
        raise QueryCheckError(
            f"query generation is not {EXPECTED_GENERATION}"
        )
    for name, expected in REQUIRED_MARKER_VALUES.items():
        if values.get(name) != expected:
            raise QueryCheckError(f"query marker {name} is not {expected}")
    parse_positive(values, "backend_pid")
    parse_positive(values, "secure_reads")
    parse_positive(values, "secure_writes")
    parse_positive(values, "socket_waits")
    parse_positive(values, "cancel_dispatches")
    configured_stack = parse_positive(
        values, "supervisor_configured_stack_bytes"
    )
    configured_guard = parse_positive(
        values, "supervisor_configured_guard_bytes"
    )
    native_stack = parse_positive(values, "supervisor_native_stack_bytes")
    native_guard = parse_positive(values, "supervisor_native_guard_bytes")
    usable_stack = parse_positive(values, "supervisor_usable_stack_bytes")
    if values.get("supervisor_actual_bounds") != "true":
        raise QueryCheckError("query supervisor stack bounds are not actual")
    if configured_stack <= configured_guard:
        raise QueryCheckError("query supervisor configured stack is invalid")
    if native_stack < configured_stack or native_guard < configured_guard:
        raise QueryCheckError("query supervisor native stack is below configuration")
    if usable_stack != native_stack - native_guard:
        raise QueryCheckError("query supervisor usable stack is inconsistent")
    return values


def supervisor_stack_from_marker(
    values: dict[str, str],
) -> dict[str, int | str]:
    return {
        "class": "supervisor",
        "configured_stack_bytes": int(
            values["supervisor_configured_stack_bytes"]
        ),
        "configured_guard_bytes": int(
            values["supervisor_configured_guard_bytes"]
        ),
        "native_stack_bytes": int(values["supervisor_native_stack_bytes"]),
        "native_guard_bytes": int(values["supervisor_native_guard_bytes"]),
        "usable_stack_bytes": int(values["supervisor_usable_stack_bytes"]),
        "actual_bounds": values["supervisor_actual_bounds"],
    }


def parse_client_quantum_count(error_output: str) -> int:
    lines = [
        line for line in error_output.splitlines()
        if "POSTGAMMA_RUNTIME" in line
    ]
    if len(lines) != 1:
        raise QueryCheckError(
            f"expected one runtime line, found {len(lines)}"
        )
    values = dict(re.findall(r"([a-z_]+)=([^\s]+)", lines[0]))
    if values.get("provider") != "pooled":
        raise QueryCheckError("query runtime did not use the pooled provider")
    try:
        quantums = int(values.get("client_quantums", ""))
    except ValueError as exc:
        raise QueryCheckError("client quantum count is not numeric") from exc
    if quantums <= 0:
        raise QueryCheckError("client quantum count is not positive")
    return quantums


def audit_trace(trace: str, driver: Path) -> dict[str, object]:
    calls = CALL_PATTERN.findall(trace)
    counts = Counter(calls)
    forbidden_signals = sorted(FORBIDDEN_SIGNAL_CALLS.intersection(counts))
    forbidden_network = sorted(FORBIDDEN_NETWORK_CALLS.intersection(counts))
    forbidden_host_state = sorted(
        FORBIDDEN_HOST_STATE_CALLS.intersection(counts)
    )
    if forbidden_signals:
        raise QueryCheckError(
            "embedded query delivered a host signal through: "
            + ", ".join(forbidden_signals)
        )
    if forbidden_network:
        raise QueryCheckError(
            "embedded query used an operating-system network endpoint through: "
            + ", ".join(forbidden_network)
        )
    if forbidden_host_state:
        raise QueryCheckError(
            "embedded query changed process-global host state through: "
            + ", ".join(forbidden_host_state)
        )
    if counts["fork"] or counts["vfork"] or counts["execveat"]:
        raise QueryCheckError("embedded query created a child process")

    exec_lines = [line for line in trace.splitlines() if "execve(" in line]
    if len(exec_lines) != 1 or str(driver) not in exec_lines[0]:
        raise QueryCheckError(
            "trace must contain only the query driver's initial execve"
        )
    clone_lines = [
        line for line in trace.splitlines() if re.search(r"\bclone3?\(", line)
    ]
    non_thread_clones = [
        line for line in clone_lines if "CLONE_THREAD" not in line
    ]
    if non_thread_clones:
        raise QueryCheckError("embedded query issued clone without CLONE_THREAD")
    if not clone_lines:
        raise QueryCheckError("embedded query did not create PostgreSQL threads")
    delivered_signals = re.findall(r"--- (SIG[A-Z0-9]+)", trace)
    if delivered_signals:
        raise QueryCheckError(
            "embedded query received real process signals: "
            + ", ".join(sorted(set(delivered_signals)))
        )
    return {
        "schema_version": 1,
        "kind": "postgamma.kernel-query-syscalls",
        "initial_execve_calls": 1,
        "thread_clone_calls": len(clone_lines),
        "process_creation_calls": 0,
        "host_signal_delivery_calls": 0,
        "network_endpoint_calls": 0,
        "process_global_cwd_calls": 0,
        "process_global_umask_calls": 0,
        "traced_syscall_count": len(calls),
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--make", default="make")
    parser.add_argument("--jobs", type=int, default=1)
    parser.add_argument("--postgres-build", required=True, type=Path)
    parser.add_argument("--driver", required=True, type=Path)
    parser.add_argument("--work-root", required=True, type=Path)
    parser.add_argument("--stdout", required=True, type=Path)
    parser.add_argument("--stderr", required=True, type=Path)
    parser.add_argument("--policy-stdout", required=True, type=Path)
    parser.add_argument("--policy-stderr", required=True, type=Path)
    parser.add_argument("--run-report", required=True, type=Path)
    parser.add_argument("--trace", required=True, type=Path)
    parser.add_argument("--trace-report", required=True, type=Path)
    parser.add_argument("--budget", required=True, type=Path)
    parser.add_argument("--input", action="append", default=[], type=Path)
    parser.add_argument("--strace", default="strace")
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()

    generated_paths = (
        args.stdout,
        args.stderr,
        args.policy_stdout,
        args.policy_stderr,
        args.run_report,
        args.trace,
        args.trace_report,
        args.output,
    )
    for path in generated_paths:
        resolved = path.resolve()
        resolved.parent.mkdir(parents=True, exist_ok=True)
        resolved.unlink(missing_ok=True)
    try:
        postgres_build = args.postgres_build.resolve(strict=True)
        driver = args.driver.resolve(strict=True)
        work_root = args.work_root.resolve()
        work_root.mkdir(parents=True, exist_ok=True)
        inputs = [path.resolve(strict=True) for path in args.input]
        budget_document = load_resource_budget(args.budget.resolve(strict=True))
        with tempfile.TemporaryDirectory(
            prefix="query-", dir=work_root
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
            data_directory = temporary_path / "data"
            run_checked(
                [
                    str(prefix / "bin" / "initdb"),
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
            config_path = data_directory / "postgresql.conf"
            base_config = config_path.read_text(encoding="utf-8")
            profile_config = (
                "\n# PostGamma embedded setting precedence evidence.\n"
                "shared_buffers = '24MB'\n"
                "max_connections = 6\n"
            )
            config_path.write_text(
                base_config
                + profile_config
                + "restart_after_crash = on\n",
                encoding="utf-8",
            )
            driver_arguments = [
                str(driver),
                str(data_directory),
                str(prefix / "bin" / "postgres"),
                str(prefix),
            ]
            rejected = run_checked(
                driver_arguments,
                environment=environment,
                timeout=90.0,
                check=False,
            )
            args.policy_stdout.write_text(rejected.stdout, encoding="utf-8")
            args.policy_stderr.write_text(rejected.stderr, encoding="utf-8")
            policy_rejection = validate_policy_rejection(
                rejected.returncode, rejected.stdout, rejected.stderr
            )
            config_path.write_text(
                base_config
                + profile_config
                + "restart_after_crash = off\n",
                encoding="utf-8",
            )
            command = [
                args.strace,
                "-f",
                "-qq",
                "-o",
                str(args.trace.resolve()),
                "-e",
                "trace=process,signal,network,chdir,fchdir,umask,setitimer",
                *driver_arguments,
            ]
            completed = run_checked(
                command, environment=environment, timeout=90.0, check=False
            )
            args.stdout.parent.mkdir(parents=True, exist_ok=True)
            args.stdout.write_text(completed.stdout, encoding="utf-8")
            args.stderr.write_text(completed.stderr, encoding="utf-8")
            if completed.returncode != 0:
                raise QueryCheckError(
                    f"command failed ({completed.returncode}): "
                    f"{' '.join(command)}\n"
                    f"stdout:\n{completed.stdout}\n"
                    f"stderr:\n{completed.stderr}"
                )
            marker = parse_marker(completed.stdout)
            supervisor_stack = validate_supervisor_stack_budget(
                budget_document, supervisor_stack_from_marker(marker)
            )
            expected_dedicated = budget_document["budgets"][
                "expected_dedicated_threads_per_cycle"
            ]
            if not isinstance(expected_dedicated, int):
                raise QueryCheckError(
                    "dedicated thread expectation is not numeric"
                )
            backend_stack = validate_backend_stack_budget(
                budget_document,
                parse_backend_stack_telemetry(
                    completed.stderr, (EXPECTED_GENERATION,)
                ),
                {
                    "dedicated": expected_dedicated,
                    "client": parse_client_quantum_count(completed.stderr),
                    "parallel": 0,
                },
                {
                    "dedicated": expected_dedicated,
                    "client": 1,
                    "parallel": 0,
                },
            )
            if (data_directory / "postmaster.pid").exists():
                raise QueryCheckError("embedded query left postmaster.pid")

        trace_report = audit_trace(args.trace.read_text(encoding="utf-8"), driver)
        write_json(args.trace_report, trace_report)
        run_report = {
            "schema_version": 1,
            "kind": "postgamma.kernel-query-run",
            "status": "pass",
            "generation": EXPECTED_GENERATION,
            "marker": marker,
            "policy_rejection": policy_rejection,
            "stack_contract": {
                "supervisor": supervisor_stack,
                "backends": backend_stack,
            },
        }
        write_json(args.run_report, run_report)
        report = {
            "schema_version": 1,
            "kind": "postgamma.kernel-query-evidence",
            "status": "pass",
            "claim": "open_memory_connect_query_error_recover_close",
            "postgresql_major": 19,
            "driver_sha256": sha256(driver),
            "inputs": input_identity(inputs),
            "run": run_report,
            "syscalls": trace_report,
            "artifacts": {
                "stdout": str(args.stdout.resolve()),
                "stderr": str(args.stderr.resolve()),
                "policy_stdout": str(args.policy_stdout.resolve()),
                "policy_stderr": str(args.policy_stderr.resolve()),
                "run_report": str(args.run_report.resolve()),
                "trace": str(args.trace.resolve()),
                "trace_report": str(args.trace_report.resolve()),
            },
        }
        write_json(args.output, report)
    except (LifecycleCheckError, QueryCheckError, OSError, ValueError) as exc:
        parser.error(str(exc))
    print(
        "embedded query evidence: pass "
        "(open -> memory connect -> SELECT/error/recover -> close)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
