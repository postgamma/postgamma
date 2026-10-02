#!/usr/bin/env python3
"""Prove the Arrow management Arrow adapter and caller-driven management API."""

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


ARROW_MARKER = "POSTGAMMA_KERNEL_3_ARROW"
MANAGEMENT_MARKER = "POSTGAMMA_KERNEL_3_MANAGEMENT"
EVIDENCE_KIND = "postgamma.arrow-management"
ARROW_VALUES = {
    "formats": "text,binary",
    "rows": "2",
    "columns": "15",
    "primitive_mapping": "true",
    "nulls": "true",
    "metadata": "true",
    "fallback": "true",
    "result_independent": "true",
    "release": "true",
    "no_arrow_runtime": "true",
    "phase": "closed",
}
MANAGEMENT_VALUES = {
    "checkpoint": "true",
    "operations": "4",
    "caller_driven": "true",
    "waitable": "true",
    "pending_cancel": "true",
    "running_cancel": "busy",
    "hidden_connections": "0",
    "lsn_advanced": "true",
    "close_gate": "true",
    "subprocesses": "0",
    "phase": "closed",
}


class ArrowManagementCheckError(RuntimeError):
    """The Arrow or management operation contract was violated."""


def fields(line: str) -> dict[str, str]:
    return dict(re.findall(r"([a-z_]+)=([^\s]+)", line))


def parse_marker(
    stdout: str, marker: str, required: dict[str, str]
) -> dict[str, str]:
    lines = [line for line in stdout.splitlines() if line.startswith(f"{marker} ")]
    if len(lines) != 1:
        raise ArrowManagementCheckError(
            f"expected one {marker} line, found {len(lines)}"
        )
    values = fields(lines[0])
    for name, expected in required.items():
        if values.get(name) != expected:
            raise ArrowManagementCheckError(
                f"{marker} field {name} is not {expected}"
            )
    return values


def parse_runtime(stderr: str, minimum_completions: int) -> dict[str, str | int]:
    lines = [line for line in stderr.splitlines() if "POSTGAMMA_RUNTIME" in line]
    if len(lines) != 1:
        raise ArrowManagementCheckError(
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
            raise ArrowManagementCheckError(
                f"runtime field {name} is not {expected}"
            )
    numeric_names = (
        "host_pid",
        "role_completions",
        "pooled_worker_threads",
        "execution_token_budget",
        "execution_tokens_peak",
    )
    try:
        numeric = {name: int(values.get(name, "")) for name in numeric_names}
    except ValueError as exc:
        raise ArrowManagementCheckError("runtime counters are invalid") from exc
    if numeric["host_pid"] <= 0:
        raise ArrowManagementCheckError("runtime host process identity is invalid")
    if numeric["role_completions"] < minimum_completions:
        raise ArrowManagementCheckError("runtime role completion count is too small")
    if numeric["pooled_worker_threads"] != 4:
        raise ArrowManagementCheckError("runtime did not use four pooled workers")
    if (
        numeric["execution_token_budget"] != 4
        or numeric["execution_tokens_peak"] <= 0
        or numeric["execution_tokens_peak"] > 4
    ):
        raise ArrowManagementCheckError("runtime execution-token bound is invalid")
    if "FATAL:" in stderr or "PANIC:" in stderr:
        raise ArrowManagementCheckError("driver emitted a fatal backend diagnostic")
    return {**values, **numeric}


def audit_arrow_boundary(
    library: Path, core_header: Path, arrow_header: Path, readelf: str
) -> dict[str, Any]:
    core = core_header.read_text(encoding="utf-8")
    arrow = arrow_header.read_text(encoding="utf-8")
    includes = re.findall(r'^\s*#\s*include\s*[<"]([^>"]+)[>"]', core, re.M)
    if any("arrow" in include.lower() for include in includes):
        raise ArrowManagementCheckError("the core header includes an Arrow header")
    if "struct ArrowSchema;" not in arrow or "struct ArrowArray;" not in arrow:
        raise ArrowManagementCheckError("the Arrow adapter lacks opaque declarations")
    dynamic = run_checked([readelf, "-d", str(library)], timeout=30.0).stdout
    needed = re.findall(r"Shared library: \[([^]]+)\]", dynamic)
    arrow_needed = [name for name in needed if "arrow" in name.lower()]
    if arrow_needed:
        raise ArrowManagementCheckError(
            "the embedded library links an Arrow runtime: " + ", ".join(arrow_needed)
        )
    return {
        "core_header_arrow_includes": 0,
        "arrow_runtime_dependencies": 0,
        "dynamic_dependencies": needed,
        "adapter_header_is_optional": True,
    }


def audit_management_boundary(
    management_source: Path,
    checkpoint_header: Path,
    instance_source: Path,
    adapter_manifest: Path,
) -> dict[str, Any]:
    management = management_source.read_text(encoding="utf-8")
    checkpoint = checkpoint_header.read_text(encoding="utf-8")
    instance = instance_source.read_text(encoding="utf-8")
    adapter = json.loads(adapter_manifest.read_text(encoding="utf-8"))
    forbidden = (
        "pgm_connection_open(",
        "pgm_execute(",
        "pgm_execute_async",
        "system(",
        "fork(",
        "execve(",
    )
    found = [token for token in forbidden if token in management]
    if found:
        raise ArrowManagementCheckError(
            "management adapter uses a forbidden execution path: "
            + ", ".join(found)
        )
    hooks = [
        hook
        for hook in adapter.get("runtime_hooks", [])
        if hook.get("id") == "pg.checkpointer.observe-embedded-checkpoint-state"
    ]
    if len(hooks) != 1 or hooks[0].get("expected_matches") != 3:
        raise ArrowManagementCheckError(
            "checkpoint completion observation has no exact semantic hook"
        )
    required_checkpoint_tokens = (
        "POSTGAMMA_CHECKPOINTER_SHMEM()",
        "checkpointer_shmem->ckpt_started",
        "checkpointer_shmem->ckpt_done",
        "checkpointer_shmem->ckpt_failed",
        "CHECKPOINT_REQUESTED",
        "postgamma_instance_checkpoint_tracker_arm(",
    )
    if any(token not in checkpoint for token in required_checkpoint_tokens):
        raise ArrowManagementCheckError(
            "checkpoint bridge does not reuse PostgreSQL checkpoint state"
        )
    required_tracker_tokens = (
        "postgamma_instance_checkpoint_tracker_create(",
        "postgamma_instance_checkpoint_observe(",
        "postgamma_instance_checkpoint_tracker_destroy(",
        "postgamma_wake_target_create(",
    )
    if any(token not in instance for token in required_tracker_tokens):
        raise ArrowManagementCheckError(
            "instance runtime lacks the reviewed checkpoint tracker lifecycle"
        )
    return {
        "hidden_sql_entrypoints": 0,
        "process_launch_entrypoints": 0,
        "semantic_observation_hooks": 3,
        "postgresql_checkpoint_counters_reused": True,
        "instance_owned_trackers": True,
    }


def prepend_library_path(environment: dict[str, str], path: Path) -> None:
    values = [str(path)]
    if environment.get("LD_LIBRARY_PATH"):
        values.append(environment["LD_LIBRARY_PATH"])
    environment["LD_LIBRARY_PATH"] = os.pathsep.join(values)


def write_text(path: Path, content: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content, encoding="utf-8")


def run_driver(
    driver: Path,
    data_directory: Path,
    prefix: Path,
    environment: dict[str, str],
    *,
    strace: str | None = None,
    trace_path: Path | None = None,
) -> Any:
    command = [
        str(driver),
        str(data_directory),
        str(prefix / "bin" / "postgres"),
        str(prefix),
        "create",
    ]
    if strace is not None:
        if trace_path is None:
            raise ArrowManagementCheckError("trace output path is missing")
        command = [
            strace,
            "-f",
            "-qq",
            "-o",
            str(trace_path),
            "-e",
            "trace=process,signal,network,chdir,fchdir,umask,setitimer",
            *command,
        ]
    return run_checked(
        command, environment=environment, timeout=300.0, check=False
    )


def require_success(name: str, completed: Any) -> None:
    if completed.returncode != 0:
        raise ArrowManagementCheckError(
            f"{name} driver failed ({completed.returncode})\n"
            f"stdout:\n{completed.stdout}\nstderr:\n{completed.stderr}"
        )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--make", default="make")
    parser.add_argument("--jobs", type=int, default=1)
    parser.add_argument("--postgres-build", required=True, type=Path)
    parser.add_argument("--library", required=True, type=Path)
    parser.add_argument("--arrow-driver", required=True, type=Path)
    parser.add_argument("--management-driver", required=True, type=Path)
    parser.add_argument("--core-header", required=True, type=Path)
    parser.add_argument("--arrow-header", required=True, type=Path)
    parser.add_argument("--management-source", required=True, type=Path)
    parser.add_argument("--checkpoint-header", required=True, type=Path)
    parser.add_argument("--instance-source", required=True, type=Path)
    parser.add_argument("--adapter-manifest", required=True, type=Path)
    parser.add_argument("--work-root", required=True, type=Path)
    parser.add_argument("--arrow-stdout", required=True, type=Path)
    parser.add_argument("--arrow-stderr", required=True, type=Path)
    parser.add_argument("--arrow-trace", required=True, type=Path)
    parser.add_argument("--management-stdout", required=True, type=Path)
    parser.add_argument("--management-stderr", required=True, type=Path)
    parser.add_argument("--management-trace", required=True, type=Path)
    parser.add_argument("--strace", default="strace")
    parser.add_argument("--readelf", default="readelf")
    parser.add_argument("--input", action="append", default=[], type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()

    generated = (
        args.arrow_stdout,
        args.arrow_stderr,
        args.arrow_trace,
        args.management_stdout,
        args.management_stderr,
        args.management_trace,
        args.output,
    )
    for path in generated:
        path.resolve().parent.mkdir(parents=True, exist_ok=True)
        path.resolve().unlink(missing_ok=True)
    try:
        postgres_build = args.postgres_build.resolve(strict=True)
        library = args.library.resolve(strict=True)
        arrow_driver = args.arrow_driver.resolve(strict=True)
        management_driver = args.management_driver.resolve(strict=True)
        core_header = args.core_header.resolve(strict=True)
        arrow_header = args.arrow_header.resolve(strict=True)
        management_source = args.management_source.resolve(strict=True)
        checkpoint_header = args.checkpoint_header.resolve(strict=True)
        instance_source = args.instance_source.resolve(strict=True)
        adapter_manifest = args.adapter_manifest.resolve(strict=True)
        inputs = [path.resolve(strict=True) for path in args.input]
        work_root = args.work_root.resolve()
        work_root.mkdir(parents=True, exist_ok=True)
        arrow_boundary = audit_arrow_boundary(
            library, core_header, arrow_header, args.readelf
        )
        management_boundary = audit_management_boundary(
            management_source, checkpoint_header, instance_source, adapter_manifest
        )
        with tempfile.TemporaryDirectory(
            prefix="api_integration-arrow-management-", dir=work_root
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
            prepend_library_path(environment, library.parent)
            environment["LC_ALL"] = "C"
            arrow = run_driver(
                arrow_driver, temporary_path / "arrow-data", prefix, environment
            )
            management = run_driver(
                management_driver,
                temporary_path / "management-data",
                prefix,
                environment,
            )
            arrow_trace = run_driver(
                arrow_driver,
                temporary_path / "arrow-trace-data",
                prefix,
                environment,
                strace=args.strace,
                trace_path=args.arrow_trace.resolve(),
            )
            management_trace = run_driver(
                management_driver,
                temporary_path / "management-trace-data",
                prefix,
                environment,
                strace=args.strace,
                trace_path=args.management_trace.resolve(),
            )
        require_success("Arrow", arrow)
        require_success("management", management)
        require_success("traced Arrow", arrow_trace)
        require_success("traced management", management_trace)
        write_text(args.arrow_stdout.resolve(), arrow.stdout)
        write_text(args.arrow_stderr.resolve(), arrow.stderr)
        write_text(args.management_stdout.resolve(), management.stdout)
        write_text(args.management_stderr.resolve(), management.stderr)
        arrow_marker = parse_marker(arrow.stdout, ARROW_MARKER, ARROW_VALUES)
        management_marker = parse_marker(
            management.stdout, MANAGEMENT_MARKER, MANAGEMENT_VALUES
        )
        traced_arrow_marker = parse_marker(
            arrow_trace.stdout, ARROW_MARKER, ARROW_VALUES
        )
        traced_management_marker = parse_marker(
            management_trace.stdout, MANAGEMENT_MARKER, MANAGEMENT_VALUES
        )
        arrow_runtime = parse_runtime(arrow.stderr, 1)
        management_runtime = parse_runtime(management.stderr, 2)
        traced_arrow_runtime = parse_runtime(arrow_trace.stderr, 1)
        traced_management_runtime = parse_runtime(management_trace.stderr, 2)
        arrow_host_safety = audit_trace(
            args.arrow_trace.resolve().read_text(encoding="utf-8"), arrow_driver
        )
        management_host_safety = audit_trace(
            args.management_trace.resolve().read_text(encoding="utf-8"),
            management_driver,
        )
        document = {
            "schema_version": 1,
            "kind": EVIDENCE_KIND,
            "status": "pass",
            "postgresql_major": 19,
            "library": str(library),
            "drivers": {
                "arrow": str(arrow_driver),
                "management": str(management_driver),
            },
            "arrow": {
                "marker": arrow_marker,
                "trace_marker": traced_arrow_marker,
                "runtime": arrow_runtime,
                "trace_runtime": traced_arrow_runtime,
                "boundary": arrow_boundary,
                "host_safety": arrow_host_safety,
            },
            "management": {
                "marker": management_marker,
                "trace_marker": traced_management_marker,
                "runtime": management_runtime,
                "trace_runtime": traced_management_runtime,
                "boundary": management_boundary,
                "host_safety": management_host_safety,
            },
            "inputs": input_identity(
                [
                    *inputs,
                    library,
                    core_header,
                    arrow_header,
                    management_source,
                    checkpoint_header,
                    instance_source,
                    adapter_manifest,
                ]
            ),
        }
        write_json(args.output.resolve(), document)
    except (
        ArrowManagementCheckError,
        LifecycleCheckError,
        OSError,
        PublicApiCheckError,
        json.JSONDecodeError,
        ValueError,
    ) as exc:
        parser.error(str(exc))
    print(
        "Arrow and management evidence: pass "
        "(optional Arrow C Data adapter, native checkpoint operation)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
