#!/usr/bin/env python3
"""Prove in-process logical backup, restore, and maintenance for logical management."""

from __future__ import annotations

import argparse
import os
import re
import shutil
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
from check_arrow_management import parse_runtime


FILE_MARKER = "POSTGAMMA_LOGICAL_TOOL"
STREAM_MARKER = "POSTGAMMA_LOGICAL_STREAM"
EVIDENCE_KIND = "postgamma.logical-management"
STREAM_SENTINEL = "/@postgamma/in-process-logical-archive"
FILE_VALUES = {
    "dumps": "2",
    "restores": "1",
    "rows": "3",
    "large_objects": "1",
    "subprocesses": "0",
    "phase": "closed",
}
STREAM_VALUES = {
    "channel_capacity": "4096",
    "progress_quantum": "1024",
    "large_objects": "1",
    "metadata": "true",
    "dependencies": "true",
    "acl": "true",
    "identity": "true",
    "clean_restore": "true",
    "invalid_flags": "true",
    "callback_failure": "true",
    "maintenance": "true",
    "maintenance_operations": "4",
    "failure_retry": "true",
    "cancel_retry": "true",
    "cancel": "true",
    "reentrant": "true",
    "stable_waitable": "true",
    "callbacks_on_progress_thread": "true",
    "concurrent_progress_busy": "true",
    "concurrent_cancel": "true",
    "running_free_cleanup": "true",
    "subprocesses": "0",
    "staging_files": "0",
    "phase": "closed",
}


class LogicalManagementCheckError(RuntimeError):
    """The logical-management product contract was violated."""


def marker_fields(stdout: str, marker: str) -> dict[str, str]:
    lines = [line for line in stdout.splitlines() if line.startswith(f"{marker} ")]
    if len(lines) != 1:
        raise LogicalManagementCheckError(
            f"expected one {marker} line, found {len(lines)}"
        )
    return dict(re.findall(r"([a-z_]+)=([^\s]+)", lines[0]))


def parse_marker(
    stdout: str, marker: str, required: dict[str, str]
) -> dict[str, str | int]:
    values = marker_fields(stdout, marker)
    for name, expected in required.items():
        if values.get(name) != expected:
            raise LogicalManagementCheckError(
                f"{marker} field {name} is not {expected}"
            )
    numeric_names = (
        ("archive_bytes", 1),
        ("pid", 1),
    )
    if marker == STREAM_MARKER:
        numeric_names += (("dump_calls", 2), ("restore_calls", 2))
    converted: dict[str, int] = {}
    try:
        for name, minimum in numeric_names:
            converted[name] = int(values.get(name, ""))
            if converted[name] < minimum:
                raise LogicalManagementCheckError(
                    f"{marker} field {name} is below {minimum}"
                )
    except ValueError as exc:
        raise LogicalManagementCheckError(
            f"{marker} contains a nonnumeric counter"
        ) from exc
    return {**values, **converted}


def write_text(path: Path, content: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content, encoding="utf-8")


def require_success(name: str, completed: Any) -> None:
    if completed.returncode != 0:
        raise LogicalManagementCheckError(
            f"{name} failed ({completed.returncode})\n"
            f"stdout:\n{completed.stdout}\nstderr:\n{completed.stderr}"
        )


def require_diagnostics_isolated(stderr: str) -> None:
    leaked = (
        "postgamma-pg-dump:",
        "postgamma-pg-restore:",
        "pg_dump:",
        "pg_restore:",
    )
    found = [token for token in leaked if token in stderr]
    if found:
        raise LogicalManagementCheckError(
            "frontend-tool diagnostics leaked to host stderr: "
            + ", ".join(found)
        )
    if "FATAL:" in stderr or "PANIC:" in stderr:
        raise LogicalManagementCheckError(
            "logical-management run emitted a fatal backend diagnostic"
        )


def traced_command(
    strace: str, trace: Path, driver: Path, arguments: list[Path]
) -> list[str]:
    return [
        strace,
        "--seccomp-bpf",
        "-f",
        "-qq",
        "-o",
        str(trace),
        "-e",
        "trace=process,signal,network,chdir,fchdir,umask,setitimer",
        str(driver),
        *(str(argument) for argument in arguments),
    ]


def filesystem_trace_command(
    strace: str, trace: Path, driver: Path, arguments: list[Path]
) -> list[str]:
    return [
        strace,
        "--seccomp-bpf",
        "-f",
        "-qq",
        "-o",
        str(trace),
        "-e",
        "trace=%file",
        "-P",
        STREAM_SENTINEL,
        str(driver),
        *(str(argument) for argument in arguments),
    ]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--make", default="make")
    parser.add_argument("--jobs", type=int, default=1)
    parser.add_argument("--postgres-build", required=True, type=Path)
    parser.add_argument("--library", required=True, type=Path)
    parser.add_argument("--file-driver", required=True, type=Path)
    parser.add_argument("--stream-driver", required=True, type=Path)
    parser.add_argument("--work-root", required=True, type=Path)
    parser.add_argument("--file-stdout", required=True, type=Path)
    parser.add_argument("--file-stderr", required=True, type=Path)
    parser.add_argument("--file-trace", required=True, type=Path)
    parser.add_argument("--stream-stdout", required=True, type=Path)
    parser.add_argument("--stream-stderr", required=True, type=Path)
    parser.add_argument("--stream-trace", required=True, type=Path)
    parser.add_argument("--stream-filesystem-trace", required=True, type=Path)
    parser.add_argument(
        "--stream-filesystem-control-trace", required=True, type=Path
    )
    parser.add_argument("--strace", default="strace")
    parser.add_argument("--input", action="append", default=[], type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()

    generated = (
        args.file_stdout,
        args.file_stderr,
        args.file_trace,
        args.stream_stdout,
        args.stream_stderr,
        args.stream_trace,
        args.stream_filesystem_trace,
        args.stream_filesystem_control_trace,
        args.output,
    )
    for path in generated:
        path.resolve().parent.mkdir(parents=True, exist_ok=True)
        path.resolve().unlink(missing_ok=True)
    try:
        postgres_build = args.postgres_build.resolve(strict=True)
        library = args.library.resolve(strict=True)
        file_driver = args.file_driver.resolve(strict=True)
        stream_driver = args.stream_driver.resolve(strict=True)
        test_program_raw = shutil.which("test")
        if test_program_raw is None:
            raise LogicalManagementCheckError(
                "cannot find a test executable for the strace positive control"
            )
        test_program = Path(test_program_raw).resolve(strict=True)
        inputs = [path.resolve(strict=True) for path in args.input]
        work_root = args.work_root.resolve()
        work_root.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(
            prefix="logical-management-", dir=work_root
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
            library_path = [str(library.parent)]
            if environment.get("LD_LIBRARY_PATH"):
                library_path.append(environment["LD_LIBRARY_PATH"])
            environment["LD_LIBRARY_PATH"] = os.pathsep.join(library_path)
            environment["LC_ALL"] = "C"
            file_archive = temporary_path / "private-regression.dump"
            file_completed = run_checked(
                traced_command(
                    args.strace,
                    args.file_trace.resolve(),
                    file_driver,
                    [
                        temporary_path / "file-data",
                        prefix / "bin" / "postgres",
                        prefix,
                        file_archive,
                    ],
                ),
                environment=environment,
                timeout=300.0,
                check=False,
            )
            stream_completed = run_checked(
                traced_command(
                    args.strace,
                    args.stream_trace.resolve(),
                    stream_driver,
                    [
                        temporary_path / "stream-data",
                        prefix / "bin" / "postgres",
                        prefix,
                    ],
                ),
                environment=environment,
                timeout=300.0,
                check=False,
            )
            filesystem_completed = run_checked(
                filesystem_trace_command(
                    args.strace,
                    args.stream_filesystem_trace.resolve(),
                    stream_driver,
                    [
                        temporary_path / "filesystem-data",
                        prefix / "bin" / "postgres",
                        prefix,
                    ],
                ),
                environment=environment,
                timeout=300.0,
                check=False,
            )
            filesystem_control_completed = run_checked(
                filesystem_trace_command(
                    args.strace,
                    args.stream_filesystem_control_trace.resolve(),
                    test_program,
                    [Path("-e"), Path(STREAM_SENTINEL)],
                ),
                environment=environment,
                timeout=30.0,
                check=False,
            )
            require_success("private logical-tool regression", file_completed)
            require_success("public logical stream", stream_completed)
            require_success(
                "logical stream filesystem sentinel", filesystem_completed
            )
            if not file_archive.is_file() or file_archive.stat().st_size == 0:
                raise LogicalManagementCheckError(
                    "private logical-tool regression produced no archive"
                )
            file_marker = parse_marker(
                file_completed.stdout, FILE_MARKER, FILE_VALUES
            )
            stream_marker = parse_marker(
                stream_completed.stdout, STREAM_MARKER, STREAM_VALUES
            )
            if file_marker["archive_bytes"] != file_archive.stat().st_size:
                raise LogicalManagementCheckError(
                    "private archive size differs from its marker"
                )
            require_diagnostics_isolated(file_completed.stderr)
            require_diagnostics_isolated(stream_completed.stderr)
            file_runtime = parse_runtime(file_completed.stderr, 3)
            stream_runtime = parse_runtime(stream_completed.stderr, 4)
            file_trace_text = args.file_trace.resolve().read_text(encoding="utf-8")
            stream_trace_text = args.stream_trace.resolve().read_text(
                encoding="utf-8"
            )
            filesystem_trace_text = (
                args.stream_filesystem_trace.resolve().read_text(encoding="utf-8")
            )
            filesystem_control_trace_text = (
                args.stream_filesystem_control_trace.resolve().read_text(
                    encoding="utf-8"
                )
            )
            file_host_safety = audit_trace(file_trace_text, file_driver)
            stream_host_safety = audit_trace(stream_trace_text, stream_driver)
            if filesystem_trace_text.strip() or STREAM_SENTINEL in stream_trace_text:
                raise LogicalManagementCheckError(
                    "the in-memory archive sentinel reached the filesystem"
                )
            if (
                filesystem_control_completed.returncode not in {0, 1}
                or not filesystem_control_trace_text.strip()
                or STREAM_SENTINEL not in filesystem_control_trace_text
            ):
                raise LogicalManagementCheckError(
                    "the filesystem sentinel trace positive control did not fire"
                )

            stream_capacity = int(stream_marker["channel_capacity"])
            dump_calls = int(stream_marker["dump_calls"])
            restore_calls = int(stream_marker["restore_calls"])
            sentinel_control_events = len(
                filesystem_control_trace_text.splitlines()
            )

        write_text(args.file_stdout.resolve(), file_completed.stdout)
        write_text(args.file_stderr.resolve(), file_completed.stderr)
        write_text(args.stream_stdout.resolve(), stream_completed.stdout)
        write_text(args.stream_stderr.resolve(), stream_completed.stderr)
        document = {
            "schema_version": 1,
            "kind": EVIDENCE_KIND,
            "status": "pass",
            "postgresql_major": 19,
            "library": str(library),
            "drivers": {
                "private_file_regression": str(file_driver),
                "public_stream": str(stream_driver),
            },
            "private_tool": {
                "marker": file_marker,
                "runtime": file_runtime,
                "host_safety": file_host_safety,
            },
            "public_api": {
                "marker": stream_marker,
                "runtime": stream_runtime,
                "host_safety": stream_host_safety,
                "archive_exceeds_channel_capacity": (
                    int(stream_marker["archive_bytes"]) > stream_capacity
                ),
                "dump_callback_calls": dump_calls,
                "restore_callback_calls": restore_calls,
                "host_diagnostic_leak_count": 0,
                "stream_sentinel_filesystem_opens": len(
                    filesystem_trace_text.splitlines()
                ),
                "sentinel_positive_control_events": sentinel_control_events,
            },
            "inputs": input_identity(
                [*inputs, library, file_driver, stream_driver]
            ),
        }
        write_json(args.output.resolve(), document)
    except (
        LifecycleCheckError,
        LogicalManagementCheckError,
        OSError,
        PublicApiCheckError,
        ValueError,
    ) as exc:
        parser.error(str(exc))
    print(
        "logical backup and restore evidence: pass "
        "(bounded in-memory archive, failure/cancel retry, no subprocess)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
