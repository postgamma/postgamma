#!/usr/bin/env python3
"""Prove PG19 durability after the complete embedded host is killed."""

from __future__ import annotations

import argparse
import os
import re
import selectors
import signal
import subprocess
import tempfile
import time
from pathlib import Path
from typing import Mapping, Sequence

from check_embedded_lifecycle import (
    LifecycleCheckError,
    input_identity,
    installed_prefix,
    run_checked,
    runtime_environment,
    sha256,
    write_json,
)
from check_embedded_query import QueryCheckError, audit_trace


READY_MARKER = "POSTGAMMA_KERNEL_CRASH_READY"
RECOVERY_MARKER = "POSTGAMMA_KERNEL_CRASH_RECOVERY"
EXPECTED_GENERATION = 620001
READY_VALUES = {
    "committed_ack": "true",
    "uncommitted_visible": "true",
    "synchronous_commit": "on",
}
RECOVERY_VALUES = {
    "committed_rows": "1",
    "uncommitted_rows": "0",
    "recovery_complete": "true",
    "transport": "memory",
    "network_calls": "0",
    "active_transports_after_close": "0",
    "endpoint_references_after_close": "0",
    "phase": "closed",
    "state": "closed",
}
RECOVERY_LOG_MESSAGES = (
    "database system was interrupted",
    "automatic recovery in progress",
    "redo starts at",
    "database system is ready to accept connections",
)


class CrashCheckError(RuntimeError):
    """The external embedded-host crash did not meet its contract."""


def parse_marker(
    output: str, marker: str, required: Mapping[str, str]
) -> dict[str, str]:
    lines = [line for line in output.splitlines() if marker in line]
    if len(lines) != 1:
        raise CrashCheckError(
            f"expected one {marker} line, found {len(lines)}"
        )
    values = dict(re.findall(r"([a-z_]+)=([^\s]+)", lines[0]))
    try:
        generation = int(values.get("generation", ""))
    except ValueError as exc:
        raise CrashCheckError(f"{marker} generation is not an integer") from exc
    if generation != EXPECTED_GENERATION:
        raise CrashCheckError(
            f"{marker} generation is not {EXPECTED_GENERATION}"
        )
    for name, expected in required.items():
        if values.get(name) != expected:
            raise CrashCheckError(
                f"{marker} value {name} is not {expected}"
            )
    for name in ("host_pid", "backend_pid"):
        try:
            value = int(values.get(name, ""))
        except ValueError as exc:
            raise CrashCheckError(f"{marker} {name} is not an integer") from exc
        if value <= 0:
            raise CrashCheckError(f"{marker} {name} is not positive")
    return values


def validate_killed_writer(
    returncode: int, process_id: int, output: str
) -> dict[str, object]:
    values = parse_marker(output, READY_MARKER, READY_VALUES)
    if int(values["host_pid"]) != process_id:
        raise CrashCheckError("ready marker does not identify the killed host")
    if returncode != -signal.SIGKILL:
        raise CrashCheckError(
            "writer host did not terminate through SIGKILL: "
            f"returncode={returncode}"
        )
    return {
        "status": "pass",
        "termination": "SIGKILL",
        "returncode": returncode,
        "process_id": process_id,
        "marker": values,
    }


def validate_recovery(
    output: str, error_output: str
) -> dict[str, object]:
    values = parse_marker(output, RECOVERY_MARKER, RECOVERY_VALUES)
    missing = [
        message for message in RECOVERY_LOG_MESSAGES
        if message not in error_output
    ]
    if missing:
        raise CrashCheckError(
            "reopen did not expose complete WAL recovery evidence: "
            + ", ".join(missing)
        )
    return {
        "status": "pass",
        "marker": values,
        "log_messages": list(RECOVERY_LOG_MESSAGES),
    }


def kill_writer_after_ready(
    arguments: Sequence[str],
    environment: Mapping[str, str],
    timeout: float,
) -> tuple[subprocess.CompletedProcess[str], int]:
    try:
        process = subprocess.Popen(
            list(arguments),
            env=environment,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            bufsize=1,
        )
    except OSError as exc:
        raise CrashCheckError(
            f"cannot execute {' '.join(arguments)}: {exc}"
        ) from exc
    if process.stdout is None or process.stderr is None:
        process.kill()
        process.wait()
        raise CrashCheckError("writer host pipes were not created")
    output_lines: list[str] = []
    selector = selectors.DefaultSelector()
    selector.register(process.stdout, selectors.EVENT_READ)
    deadline = time.monotonic() + timeout
    ready = False
    try:
        while not ready:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise CrashCheckError("writer host did not reach crash barrier")
            events = selector.select(min(remaining, 0.5))
            for key, _ in events:
                line = key.fileobj.readline()
                if line:
                    output_lines.append(line)
                    if READY_MARKER in line:
                        ready = True
                        break
            if process.poll() is not None and not ready:
                tail, error_output = process.communicate()
                output_lines.append(tail)
                raise CrashCheckError(
                    "writer host exited before crash barrier: "
                    f"returncode={process.returncode}\n"
                    f"stdout:\n{''.join(output_lines)}\n"
                    f"stderr:\n{error_output}"
                )
        parse_marker("".join(output_lines), READY_MARKER, READY_VALUES)
        process.kill()
        tail, error_output = process.communicate(timeout=15.0)
        output_lines.append(tail)
    except (OSError, subprocess.TimeoutExpired) as exc:
        raise CrashCheckError(f"cannot terminate writer host: {exc}") from exc
    finally:
        selector.close()
        if process.poll() is None:
            process.kill()
            process.communicate()
    completed = subprocess.CompletedProcess(
        list(arguments), process.returncode, "".join(output_lines), error_output
    )
    return completed, process.pid


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--make", default="make")
    parser.add_argument("--jobs", type=int, default=1)
    parser.add_argument("--postgres-build", required=True, type=Path)
    parser.add_argument("--driver", required=True, type=Path)
    parser.add_argument("--work-root", required=True, type=Path)
    parser.add_argument("--writer-stdout", required=True, type=Path)
    parser.add_argument("--writer-stderr", required=True, type=Path)
    parser.add_argument("--verifier-stdout", required=True, type=Path)
    parser.add_argument("--verifier-stderr", required=True, type=Path)
    parser.add_argument("--run-report", required=True, type=Path)
    parser.add_argument("--trace", required=True, type=Path)
    parser.add_argument("--trace-report", required=True, type=Path)
    parser.add_argument("--input", action="append", default=[], type=Path)
    parser.add_argument("--strace", default="strace")
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()

    generated_paths = (
        args.writer_stdout,
        args.writer_stderr,
        args.verifier_stdout,
        args.verifier_stderr,
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
        inputs = [path.resolve(strict=True) for path in args.input]
        work_root = args.work_root.resolve()
        work_root.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(
            prefix="crash-", dir=work_root
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
            config_path.write_text(
                config_path.read_text(encoding="utf-8")
                + "\n# PostGamma cluster lifecycle crash oracle.\n"
                + "synchronous_commit = on\n"
                + "fsync = on\n"
                + "full_page_writes = on\n",
                encoding="utf-8",
            )
            common_arguments = [
                str(data_directory),
                str(prefix / "bin" / "postgres"),
                str(prefix),
            ]
            writer, writer_pid = kill_writer_after_ready(
                [str(driver), "crash-writer", *common_arguments],
                environment,
                90.0,
            )
            args.writer_stdout.write_text(writer.stdout, encoding="utf-8")
            args.writer_stderr.write_text(writer.stderr, encoding="utf-8")
            writer_report = validate_killed_writer(
                writer.returncode, writer_pid, writer.stdout
            )
            if (data_directory / "postmaster.pid").exists():
                raise CrashCheckError("killed embedded host left postmaster.pid")
            verifier_command = [
                args.strace,
                "-f",
                "-qq",
                "-o",
                str(args.trace.resolve()),
                "-e",
                "trace=process,signal,network,chdir,fchdir,umask,setitimer",
                str(driver),
                "crash-verifier",
                *common_arguments,
            ]
            verifier = run_checked(
                verifier_command,
                environment=environment,
                timeout=90.0,
                check=False,
            )
            args.verifier_stdout.write_text(
                verifier.stdout, encoding="utf-8"
            )
            args.verifier_stderr.write_text(
                verifier.stderr, encoding="utf-8"
            )
            if verifier.returncode != 0:
                raise CrashCheckError(
                    f"recovery verifier failed ({verifier.returncode})\n"
                    f"stdout:\n{verifier.stdout}\n"
                    f"stderr:\n{verifier.stderr}"
                )
            recovery_report = validate_recovery(
                verifier.stdout, verifier.stderr
            )
            if (data_directory / "postmaster.pid").exists():
                raise CrashCheckError("recovered embedded host left postmaster.pid")

        trace_report = audit_trace(
            args.trace.read_text(encoding="utf-8"), driver
        )
        trace_report["kind"] = "postgamma.crash-recovery-syscalls"
        write_json(args.trace_report, trace_report)
        run_report = {
            "schema_version": 1,
            "kind": "postgamma.crash-recovery-run",
            "status": "pass",
            "writer": writer_report,
            "recovery": recovery_report,
        }
        write_json(args.run_report, run_report)
        report = {
            "schema_version": 1,
            "kind": "postgamma.crash-recovery-evidence",
            "status": "pass",
            "claim": "acknowledged_commit_durable_uncommitted_transaction_lost",
            "postgresql_major": 19,
            "driver_sha256": sha256(driver),
            "inputs": input_identity(inputs),
            "run": run_report,
            "reopen_syscalls": trace_report,
            "artifacts": {
                "writer_stdout": str(args.writer_stdout.resolve()),
                "writer_stderr": str(args.writer_stderr.resolve()),
                "verifier_stdout": str(args.verifier_stdout.resolve()),
                "verifier_stderr": str(args.verifier_stderr.resolve()),
                "run_report": str(args.run_report.resolve()),
                "trace": str(args.trace.resolve()),
                "trace_report": str(args.trace_report.resolve()),
            },
        }
        write_json(args.output, report)
    except (
        CrashCheckError,
        LifecycleCheckError,
        QueryCheckError,
        OSError,
        ValueError,
    ) as exc:
        parser.error(str(exc))
    print(
        "embedded crash evidence: pass "
        "(acknowledged commit -> host SIGKILL -> WAL recovery -> verify)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
