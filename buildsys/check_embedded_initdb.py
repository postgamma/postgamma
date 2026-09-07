#!/usr/bin/env python3
"""Prove atomic, host-safe, in-process PostgreSQL 19 cluster creation."""

from __future__ import annotations

import argparse
import errno
import os
import re
import selectors
import signal
import subprocess
import tempfile
import time
from pathlib import Path

from check_embedded_lifecycle import (
    LifecycleCheckError,
    input_identity,
    installed_prefix,
    run_checked,
    runtime_environment,
    write_json,
)
from check_embedded_public_api import PublicApiCheckError, audit_trace


INITDB_MARKER = "POSTGAMMA_INITDB"
CLUSTER_MARKER = "POSTGAMMA_CLUSTER_CREATE"
PRE_PUBLISH_FAULT_MODES = (
    "fault-temporary",
    "fault-bootstrap",
    "fault-post-bootstrap",
    "fault-initdb",
    "fault-publish",
)
POST_PUBLISH_FAULT_MODES = (
    "fault-published",
    "fault-owner-removed",
)
FAULT_MODES = PRE_PUBLISH_FAULT_MODES + POST_PUBLISH_FAULT_MODES
CRASH_MODES = (
    ("crash-temporary", "temporary-created", False, 2),
    ("crash-bootstrap", "bootstrap-complete", False, 2),
    ("crash-post-bootstrap", "post-bootstrap-complete", False, 2),
    ("crash-initdb", "initdb-complete", False, 2),
    ("crash-publish", "publish-ready", False, 2),
    ("crash-published", "published", True, 1),
    ("crash-owner-removed", "owner-removed", True, 0),
)
EXPECTED_CLUSTER_VALUES = {
    "clusters": "2",
    "unique_identifiers": "true",
    "same_process": "true",
    "reopen": "true",
    "plpgsql": "true",
    "snowball": "true",
    "checksums": "true",
    "phase": "closed",
}


class InitdbCheckError(RuntimeError):
    """The in-process cluster creation contract was not satisfied."""


def parse_values(line: str) -> dict[str, str]:
    return dict(re.findall(r"([a-z_]+)=([^\s]+)", line))


def require_marker(stdout: str, marker: str) -> dict[str, str]:
    lines = [line for line in stdout.splitlines() if marker in line]
    if len(lines) != 1:
        raise InitdbCheckError(
            f"expected one {marker} line, found {len(lines)}"
        )
    return parse_values(lines[0])


def staging_directories(target: Path) -> list[Path]:
    prefix = f".{target.name}.postgamma-create."
    if not target.parent.is_dir():
        return []
    return sorted(
        path
        for path in target.parent.iterdir()
        if path.name.startswith(prefix)
        and len(path.name) == len(prefix) + 6
        and path.is_dir()
        and not path.is_symlink()
    )


def staging_artifacts(target: Path) -> list[Path]:
    prefix = f".{target.name}.postgamma-create."
    if not target.parent.is_dir():
        return []
    return sorted(
        path
        for path in target.parent.iterdir()
        if (
            path.name.startswith(prefix)
            and len(path.name) == len(prefix) + 6
            and path.is_dir()
            and not path.is_symlink()
        )
        or (
            path.name.startswith(prefix)
            and len(path.name) == len(prefix) + 6 + len(".owner")
            and path.name.endswith(".owner")
            and path.is_file()
            and not path.is_symlink()
        )
    )


def require_no_staging(target: Path) -> None:
    staged = staging_artifacts(target)
    if staged:
        raise InitdbCheckError(
            "cluster creation left temporary directories: "
            + ", ".join(path.name for path in staged)
        )


def require_cluster(target: Path) -> None:
    try:
        version = (target / "PG_VERSION").read_text(encoding="ascii").strip()
    except OSError as exc:
        raise InitdbCheckError(f"created cluster has no PG_VERSION: {exc}") from exc
    if version != "19" or not (target / "global" / "pg_control").is_file():
        raise InitdbCheckError("created destination is not a PostgreSQL 19 cluster")
    if (target / "postmaster.pid").exists():
        raise InitdbCheckError("created cluster contains a standard postmaster.pid")


def require_absent(target: Path) -> None:
    if target.exists() or target.is_symlink():
        raise InitdbCheckError(f"failed creation published target {target}")


def run_private(
    driver: Path,
    mode: str,
    target: Path,
    postgres: Path,
    prefix: Path,
    environment: dict[str, str],
) -> subprocess.CompletedProcess[str]:
    completed = run_checked(
        [str(driver), mode, str(target), str(postgres), str(prefix)],
        environment=environment,
        timeout=90.0,
        check=False,
    )
    if completed.returncode != 0:
        raise InitdbCheckError(
            f"private initdb mode {mode} failed ({completed.returncode})\n"
            f"stdout:\n{completed.stdout}\nstderr:\n{completed.stderr}"
        )
    values = require_marker(completed.stdout, INITDB_MARKER)
    if values.get("mode") not in {
        mode,
        "fault" if mode.startswith("fault-") else mode,
    }:
        raise InitdbCheckError(f"private initdb mode marker mismatch for {mode}")
    return completed


def run_crash_oracle(
    driver: Path,
    mode: str,
    point: str,
    expect_published: bool,
    expected_artifacts: int,
    target: Path,
    postgres: Path,
    prefix: Path,
    environment: dict[str, str],
) -> tuple[str, str, int]:
    process = subprocess.Popen(
        [
            str(driver),
            mode,
            str(target),
            str(postgres),
            str(prefix),
        ],
        env=environment,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        bufsize=1,
    )
    assert process.stdout is not None
    selector = selectors.DefaultSelector()
    selector.register(process.stdout, selectors.EVENT_READ)
    stdout_parts: list[str] = []
    ready = False
    ready_marker = f"POSTGAMMA_INITDB mode=crash-ready point={point}"
    deadline = time.monotonic() + 30.0
    try:
        while time.monotonic() < deadline:
            if process.poll() is not None:
                break
            events = selector.select(timeout=0.1)
            for key, _mask in events:
                line = key.fileobj.readline()
                if not line:
                    continue
                stdout_parts.append(line)
                if ready_marker in line:
                    ready = True
                    break
            if ready:
                break
        if not ready:
            process.kill()
            stdout_tail, stderr = process.communicate(timeout=10.0)
            raise InitdbCheckError(
                f"crash oracle did not reach the {point} barrier\n"
                f"stdout:{''.join(stdout_parts)}{stdout_tail}\nstderr:{stderr}"
            )
        process.kill()
        stdout_tail, stderr = process.communicate(timeout=10.0)
    finally:
        selector.close()
        if process.poll() is None:
            process.kill()
            process.wait(timeout=10.0)
    stdout = "".join(stdout_parts) + stdout_tail
    if process.returncode != -signal.SIGKILL:
        raise InitdbCheckError(
            f"crash oracle host exited with {process.returncode}, expected SIGKILL"
        )
    if expect_published:
        require_cluster(target)
    else:
        require_absent(target)
    artifacts = staging_artifacts(target)
    if len(artifacts) != expected_artifacts:
        raise InitdbCheckError(
            f"crash oracle {point} expected {expected_artifacts} staging "
            f"artifacts, found {len(artifacts)}"
        )
    owners = [path for path in artifacts if path.name.endswith(".owner")]
    if expected_artifacts != 0:
        if len(owners) != 1 or owners[0].read_text(
            encoding="utf-8"
        ) != f"{target}\n":
            raise InitdbCheckError(
                f"crash oracle {point} has no valid staging owner record"
            )
    return stdout, stderr, len(artifacts)


def run_process_contention(
    driver: Path,
    target: Path,
    postgres: Path,
    prefix: Path,
    environment: dict[str, str],
) -> tuple[str, str]:
    command = [
        str(driver),
        "contended",
        str(target),
        str(postgres),
        str(prefix),
    ]
    processes = [
        subprocess.Popen(
            command,
            env=environment,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        for _index in range(2)
    ]
    completed: list[tuple[int, str, str]] = []
    try:
        for process in processes:
            stdout, stderr = process.communicate(timeout=90.0)
            completed.append((process.returncode, stdout, stderr))
    finally:
        for process in processes:
            if process.poll() is None:
                process.kill()
                process.wait(timeout=10.0)
    failures = [record for record in completed if record[0] != 0]
    if failures:
        raise InitdbCheckError(
            "cross-process cluster creation failed\n"
            + "\n".join(
                f"status={status}\nstdout:{stdout}\nstderr:{stderr}"
                for status, stdout, stderr in failures
            )
        )
    values = [require_marker(stdout, INITDB_MARKER) for _, stdout, _ in completed]
    created_count = sum(value.get("created") == "true" for value in values)
    if created_count != 1:
        raise InitdbCheckError(
            "cross-process contention did not produce exactly one publisher"
        )
    return (
        "".join(stdout for _, stdout, _ in completed),
        "".join(stderr for _, _, stderr in completed),
    )


def audit_runtime(stderr: str) -> dict[str, int | bool]:
    lines = [line for line in stderr.splitlines() if "POSTGAMMA_RUNTIME" in line]
    if len(lines) != 3:
        raise InitdbCheckError(
            f"expected three same-process runtime records, found {len(lines)}"
        )
    total_roles = 0
    for line in lines:
        values = parse_values(line)
        required = {
            "backend_model": "thread",
            "threads_started": "true",
            "role_process_launches": "0",
            "forbidden_process_launch_attempts": "0",
            "unsupported_role_requests": "0",
            "role_threads_active": "0",
        }
        for name, expected in required.items():
            if values.get(name) != expected:
                raise InitdbCheckError(
                    f"cluster runtime record {name} is not {expected}"
                )
        try:
            total_roles += int(values["role_completions"])
        except (KeyError, ValueError) as exc:
            raise InitdbCheckError("invalid role completion telemetry") from exc
    if "FATAL:" in stderr or "PANIC:" in stderr:
        raise InitdbCheckError("cluster creation run emitted a fatal backend error")
    return {
        "runtime_records": len(lines),
        "thread_model": True,
        "total_role_completions": total_roles,
        "role_threads_active_after_each_close": 0,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--make", default="make")
    parser.add_argument("--jobs", type=int, default=1)
    parser.add_argument("--postgres-build", required=True, type=Path)
    parser.add_argument("--private-driver", required=True, type=Path)
    parser.add_argument("--cluster-driver", required=True, type=Path)
    parser.add_argument("--work-root", required=True, type=Path)
    parser.add_argument("--initdb-stdout", required=True, type=Path)
    parser.add_argument("--initdb-stderr", required=True, type=Path)
    parser.add_argument("--cluster-stdout", required=True, type=Path)
    parser.add_argument("--cluster-stderr", required=True, type=Path)
    parser.add_argument("--trace", required=True, type=Path)
    parser.add_argument("--run-report", required=True, type=Path)
    parser.add_argument("--input", action="append", default=[], type=Path)
    parser.add_argument("--strace", default="strace")
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()

    generated = (
        args.initdb_stdout,
        args.initdb_stderr,
        args.cluster_stdout,
        args.cluster_stderr,
        args.trace,
        args.run_report,
        args.output,
    )
    for path in generated:
        path.resolve().parent.mkdir(parents=True, exist_ok=True)
        path.resolve().unlink(missing_ok=True)
    started = time.monotonic()
    try:
        postgres_build = args.postgres_build.resolve(strict=True)
        private_driver = args.private_driver.resolve(strict=True)
        cluster_driver = args.cluster_driver.resolve(strict=True)
        inputs = [path.resolve(strict=True) for path in args.input]
        work_root = args.work_root.resolve()
        work_root.mkdir(parents=True, exist_ok=True)
        direct_stdout: list[str] = []
        direct_stderr: list[str] = []
        with tempfile.TemporaryDirectory(
            prefix="initdb-create-", dir=work_root
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
            postgres = prefix / "bin" / "postgres"
            environment = runtime_environment(prefix)
            environment["LC_ALL"] = "C"

            normal = temporary_path / "normal"
            for mode in ("create", "existing"):
                completed = run_private(
                    private_driver, mode, normal, postgres, prefix, environment
                )
                direct_stdout.append(completed.stdout)
                direct_stderr.append(completed.stderr)
            require_cluster(normal)
            require_no_staging(normal)

            host_stream = temporary_path / "host-stream"
            completed = run_private(
                private_driver,
                "host-stream",
                host_stream,
                postgres,
                prefix,
                environment,
            )
            direct_stdout.append(completed.stdout)
            direct_stderr.append(completed.stderr)
            require_cluster(host_stream)
            require_no_staging(host_stream)

            empty = temporary_path / "empty"
            empty.mkdir()
            completed = run_private(
                private_driver, "create", empty, postgres, prefix, environment
            )
            direct_stdout.append(completed.stdout)
            direct_stderr.append(completed.stderr)
            require_cluster(empty)
            require_no_staging(empty)

            owned_target = temporary_path / "owned-cleanup-safety"
            unowned_staging = (
                temporary_path
                / ".owned-cleanup-safety.postgamma-create.A1b2C3"
            )
            unowned_staging.mkdir()
            unowned_sentinel = unowned_staging / "host-owned.txt"
            unowned_sentinel.write_text("preserve\n", encoding="ascii")
            completed = run_private(
                private_driver,
                "create",
                owned_target,
                postgres,
                prefix,
                environment,
            )
            direct_stdout.append(completed.stdout)
            direct_stderr.append(completed.stderr)
            require_cluster(owned_target)
            if unowned_sentinel.read_text(encoding="ascii") != "preserve\n":
                raise InitdbCheckError("stale cleanup removed an unowned directory")

            nonempty = temporary_path / "nonempty"
            nonempty.mkdir()
            sentinel = nonempty / "host-owned.txt"
            sentinel.write_text("preserve\n", encoding="ascii")
            completed = run_private(
                private_driver, "reject", nonempty, postgres, prefix, environment
            )
            direct_stdout.append(completed.stdout)
            direct_stderr.append(completed.stderr)
            if sentinel.read_text(encoding="ascii") != "preserve\n":
                raise InitdbCheckError("nonempty destination was modified")

            rejected_umask = temporary_path / "rejected-umask"
            completed = run_private(
                private_driver,
                "reject-umask",
                rejected_umask,
                postgres,
                prefix,
                environment,
            )
            direct_stdout.append(completed.stdout)
            direct_stderr.append(completed.stderr)
            require_absent(rejected_umask)
            require_no_staging(rejected_umask)

            concurrent = temporary_path / "concurrent"
            completed = run_private(
                private_driver,
                "concurrent",
                concurrent,
                postgres,
                prefix,
                environment,
            )
            direct_stdout.append(completed.stdout)
            direct_stderr.append(completed.stderr)
            if require_marker(completed.stdout, INITDB_MARKER).get(
                "created_count"
            ) != "1":
                raise InitdbCheckError("concurrent creation had multiple publishers")
            require_cluster(concurrent)
            require_no_staging(concurrent)

            independent_root = temporary_path / "concurrent-independent"
            completed = run_private(
                private_driver,
                "concurrent-independent",
                independent_root,
                postgres,
                prefix,
                environment,
            )
            direct_stdout.append(completed.stdout)
            direct_stderr.append(completed.stderr)
            if require_marker(completed.stdout, INITDB_MARKER).get(
                "created_count"
            ) != "2":
                raise InitdbCheckError(
                    "independent concurrent creation did not publish both clusters"
                )
            for parent_name in ("parent-a", "parent-b"):
                independent_target = (
                    independent_root / parent_name / "cluster"
                )
                require_cluster(independent_target)
                require_no_staging(independent_target)

            lock_timeout = temporary_path / "lock-timeout"
            completed = run_private(
                private_driver,
                "lock-timeout",
                lock_timeout,
                postgres,
                prefix,
                environment,
            )
            direct_stdout.append(completed.stdout)
            direct_stderr.append(completed.stderr)
            require_absent(lock_timeout)
            require_no_staging(lock_timeout)

            lock_handoff = temporary_path / "lock-handoff"
            completed = run_private(
                private_driver,
                "lock-handoff",
                lock_handoff,
                postgres,
                prefix,
                environment,
            )
            direct_stdout.append(completed.stdout)
            direct_stderr.append(completed.stderr)
            require_cluster(lock_handoff)
            require_no_staging(lock_handoff)

            process_target = temporary_path / "process-contention"
            process_stdout, process_stderr = run_process_contention(
                private_driver,
                process_target,
                postgres,
                prefix,
                environment,
            )
            direct_stdout.append(process_stdout)
            direct_stderr.append(process_stderr)
            require_cluster(process_target)
            require_no_staging(process_target)

            for mode in FAULT_MODES:
                target = temporary_path / mode
                completed = run_private(
                    private_driver, mode, target, postgres, prefix, environment
                )
                direct_stdout.append(completed.stdout)
                direct_stderr.append(completed.stderr)
                values = require_marker(completed.stdout, INITDB_MARKER)
                if values.get("status") != str(errno.ECANCELED):
                    raise InitdbCheckError(f"fault mode {mode} did not preserve errno")
                if mode in POST_PUBLISH_FAULT_MODES:
                    require_cluster(target)
                else:
                    require_absent(target)
                require_no_staging(target)

            missing_resource_target = temporary_path / "missing-resource-target"
            completed = run_private(
                private_driver,
                "reject",
                missing_resource_target,
                postgres,
                temporary_path / "missing-resource",
                environment,
            )
            direct_stdout.append(completed.stdout)
            direct_stderr.append(completed.stderr)
            require_absent(missing_resource_target)
            require_no_staging(missing_resource_target)

            crash_artifact_counts: dict[str, int] = {}
            for mode, point, published, expected_artifacts in CRASH_MODES:
                crash_target = temporary_path / mode
                crash_stdout, crash_stderr, artifact_count = run_crash_oracle(
                    private_driver,
                    mode,
                    point,
                    published,
                    expected_artifacts,
                    crash_target,
                    postgres,
                    prefix,
                    environment,
                )
                crash_artifact_counts[point] = artifact_count
                direct_stdout.append(crash_stdout)
                direct_stderr.append(crash_stderr)
                retry_mode = "existing" if published else "create"
                completed = run_private(
                    private_driver,
                    retry_mode,
                    crash_target,
                    postgres,
                    prefix,
                    environment,
                )
                direct_stdout.append(completed.stdout)
                direct_stderr.append(completed.stderr)
                retry_values = require_marker(completed.stdout, INITDB_MARKER)
                if retry_values.get("created") != (
                    "false" if published else "true"
                ):
                    raise InitdbCheckError(
                        f"crash retry publication mismatch for {point}"
                    )
                require_cluster(crash_target)
                require_no_staging(crash_target)

            cluster_root = temporary_path / "cluster-pair"
            cluster_root.mkdir()
            command = [
                args.strace,
                "-f",
                "-qq",
                "-o",
                str(args.trace.resolve()),
                "-e",
                "trace=process,signal,network,chdir,fchdir,umask,setitimer",
                str(cluster_driver),
                str(cluster_root),
                str(postgres),
                str(prefix),
            ]
            completed = run_checked(
                command, environment=environment, timeout=180.0, check=False
            )
            args.cluster_stdout.resolve().write_text(
                completed.stdout, encoding="utf-8"
            )
            args.cluster_stderr.resolve().write_text(
                completed.stderr, encoding="utf-8"
            )
            if completed.returncode != 0:
                raise InitdbCheckError(
                    f"cluster pair driver failed ({completed.returncode})\n"
                    f"stdout:\n{completed.stdout}\nstderr:\n{completed.stderr}"
                )
            cluster_values = require_marker(completed.stdout, CLUSTER_MARKER)
            for name, expected in EXPECTED_CLUSTER_VALUES.items():
                if cluster_values.get(name) != expected:
                    raise InitdbCheckError(
                        f"cluster creation marker {name} is not {expected}"
                    )
            runtime = audit_runtime(completed.stderr)
            trace = args.trace.resolve().read_text(encoding="utf-8")
            host_safety = audit_trace(trace, cluster_driver)
            require_cluster(cluster_root / "cluster-a")
            require_cluster(cluster_root / "cluster-b")
            require_no_staging(cluster_root / "cluster-a")
            require_no_staging(cluster_root / "cluster-b")

        args.initdb_stdout.resolve().write_text(
            "".join(direct_stdout), encoding="utf-8"
        )
        args.initdb_stderr.resolve().write_text(
            "".join(direct_stderr), encoding="utf-8"
        )
        elapsed_ms = round((time.monotonic() - started) * 1000)
        run_report = {
            "schema_version": 1,
            "kind": "postgamma.initdb-create-run",
            "status": "pass",
            "elapsed_ms": elapsed_ms,
            "normal_create": True,
            "existing_idempotent": True,
            "host_stdio_isolated": True,
            "nondefault_creation_umask_rejected": True,
            "empty_destination": True,
            "nonempty_destination_preserved": True,
            "unowned_staging_preserved": True,
            "concurrent_publishers": 1,
            "independent_concurrent_publishers": 2,
            "cross_process_publishers": 1,
            "bounded_creation_lock": True,
            "instance_lock_handoff": True,
            "fault_points": list(FAULT_MODES),
            "crash_points": [point for _mode, point, _published, _count in CRASH_MODES],
            "crash_artifact_counts_before_retry": crash_artifact_counts,
            "crash_retries_cleaned_staging": True,
        }
        write_json(args.run_report.resolve(), run_report)
        document = {
            "schema_version": 1,
            "kind": "postgamma.in-process-cluster-creation",
            "status": "pass",
            "postgresql_major": 19,
            "creation": {
                "subprocesses": 0,
                "external_initdb_invocations": 0,
                "atomic_publish": True,
                "existing_idempotent": True,
                "host_stdio_isolated": True,
                "logical_umask": "0077-only",
                "empty_destination_supported": True,
                "nonempty_destination_preserved": True,
                "unowned_staging_preserved": True,
                "concurrent_publishers": 1,
                "independent_concurrent_publishers": 2,
                "cross_process_publishers": 1,
                "creation_lock_deadline_bounded": True,
                "instance_lock_handoff": True,
            },
            "fault_rollback": {
                "point_count": len(FAULT_MODES),
                "points": list(FAULT_MODES),
                "pre_publish_point_count": len(PRE_PUBLISH_FAULT_MODES),
                "post_publish_point_count": len(POST_PUBLISH_FAULT_MODES),
                "published_partial_clusters": 0,
                "temporary_directories_after_return": 0,
            },
            "crash_oracle": {
                "host_termination": "SIGKILL",
                "point_count": len(CRASH_MODES),
                "points": [
                    point for _mode, point, _published, _count in CRASH_MODES
                ],
                "pre_and_post_publish_windows": True,
                "artifact_counts_before_retry": crash_artifact_counts,
                "retry_removed_abandoned_staging": True,
            },
            "same_process": {
                "clusters_created": 2,
                "unique_system_identifiers": True,
                "reopen_preserved_identifier": True,
                "plpgsql_static_module": True,
                "snowball_static_module": True,
            },
            "runtime": runtime,
            "host_safety": host_safety,
            "inputs": input_identity(inputs),
        }
        write_json(args.output.resolve(), document)
    except (
        LifecycleCheckError,
        OSError,
        PublicApiCheckError,
        InitdbCheckError,
        subprocess.SubprocessError,
    ) as exc:
        parser.error(str(exc))
    print(
        "embedded in-process cluster creation evidence: pass "
        "(atomic create, 7 fault points, 7 SIGKILL windows, "
        "2 unique clusters)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
