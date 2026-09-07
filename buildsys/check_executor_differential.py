#!/usr/bin/env python3
"""Compare real PG19 SQL behavior across pooled and dedicated executors."""

from __future__ import annotations

import argparse
import re
import tempfile
from pathlib import Path

from check_embedded_lifecycle import (
    LifecycleCheckError,
    input_identity,
    installed_prefix,
    run_checked,
    runtime_environment,
    write_json,
)
from check_embedded_query import REQUIRED_MARKER_VALUES


MARKER = "POSTGAMMA_KERNEL_QUERY"
SEMANTIC_FIELDS = tuple(REQUIRED_MARKER_VALUES)


class ExecutorDifferentialError(RuntimeError):
    """The executor implementations did not produce equivalent behavior."""


def parse_marker(output: str, expected_executor: str) -> dict[str, str]:
    lines = [line for line in output.splitlines() if MARKER in line]
    if len(lines) != 1:
        raise ExecutorDifferentialError(
            f"expected one {MARKER} line, found {len(lines)}"
        )
    values = dict(re.findall(r"([a-z_]+)=([^\s]+)", lines[0]))
    for name, expected in REQUIRED_MARKER_VALUES.items():
        if name == "executor":
            expected = expected_executor
        if values.get(name) != expected:
            raise ExecutorDifferentialError(
                f"{expected_executor} marker {name} is not {expected}"
            )
    return values


def parse_runtime(stderr: str, expected_provider: str) -> dict[str, str | int]:
    lines = [line for line in stderr.splitlines() if "POSTGAMMA_RUNTIME" in line]
    if len(lines) != 1:
        raise ExecutorDifferentialError(
            f"expected one runtime line, found {len(lines)}"
        )
    values = dict(re.findall(r"([a-z_]+)=([^\s]+)", lines[0]))
    if values.get("provider") != expected_provider:
        raise ExecutorDifferentialError(
            f"runtime provider is not {expected_provider}"
        )
    try:
        quantums = int(values.get("client_quantums", ""))
        yields = int(values.get("quantum_yields", ""))
        migrations = int(values.get("carrier_migrations", ""))
        active = int(values.get("role_threads_active", ""))
    except ValueError as exc:
        raise ExecutorDifferentialError("runtime counters are invalid") from exc
    if active != 0:
        raise ExecutorDifferentialError("executor leaked an active role")
    if expected_provider == "pooled" and (
        quantums <= 1 or yields == 0 or migrations == 0
    ):
        raise ExecutorDifferentialError("pooled run did not migrate sessions")
    if expected_provider == "dedicated" and (
        quantums != 0 or yields != 0 or migrations != 0
    ):
        raise ExecutorDifferentialError("dedicated Oracle used pooled quantums")
    if "FATAL:" in stderr or "PANIC:" in stderr:
        raise ExecutorDifferentialError("executor run emitted FATAL or PANIC")
    return {
        **values,
        "client_quantums": quantums,
        "quantum_yields": yields,
        "carrier_migrations": migrations,
        "role_threads_active": active,
    }


def configure_cluster(
    initdb: Path, data_directory: Path, environment: dict[str, str]
) -> None:
    run_checked(
        [
            str(initdb),
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
        + "\nshared_buffers = '24MB'\n"
        + "max_connections = 6\n"
        + "restart_after_crash = off\n",
        encoding="utf-8",
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--make", default="make")
    parser.add_argument("--jobs", type=int, default=1)
    parser.add_argument("--postgres-build", required=True, type=Path)
    parser.add_argument("--driver", required=True, type=Path)
    parser.add_argument("--work-root", required=True, type=Path)
    parser.add_argument("--pooled-stdout", required=True, type=Path)
    parser.add_argument("--pooled-stderr", required=True, type=Path)
    parser.add_argument("--dedicated-stdout", required=True, type=Path)
    parser.add_argument("--dedicated-stderr", required=True, type=Path)
    parser.add_argument("--input", action="append", default=[], type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()

    generated = (
        args.pooled_stdout,
        args.pooled_stderr,
        args.dedicated_stdout,
        args.dedicated_stderr,
        args.output,
    )
    for path in generated:
        path.resolve().parent.mkdir(parents=True, exist_ok=True)
        path.resolve().unlink(missing_ok=True)
    try:
        postgres_build = args.postgres_build.resolve(strict=True)
        driver = args.driver.resolve(strict=True)
        inputs = [path.resolve(strict=True) for path in args.input]
        work_root = args.work_root.resolve()
        work_root.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="session-oracle-", dir=work_root) as temp:
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
            runs: dict[str, dict[str, object]] = {}
            for executor in ("pooled", "dedicated"):
                data_directory = temporary / executor
                configure_cluster(prefix / "bin" / "initdb", data_directory, environment)
                command = [str(driver)]
                if executor == "dedicated":
                    command.append("dedicated-query")
                command.extend(
                    [
                        str(data_directory),
                        str(prefix / "bin" / "postgres"),
                        str(prefix),
                    ]
                )
                completed = run_checked(
                    command, environment=environment, timeout=120.0, check=False
                )
                stdout_path = (
                    args.pooled_stdout if executor == "pooled" else args.dedicated_stdout
                )
                stderr_path = (
                    args.pooled_stderr if executor == "pooled" else args.dedicated_stderr
                )
                stdout_path.resolve().write_text(completed.stdout, encoding="utf-8")
                stderr_path.resolve().write_text(completed.stderr, encoding="utf-8")
                if completed.returncode != 0:
                    raise ExecutorDifferentialError(
                        f"{executor} executor failed ({completed.returncode})\n"
                        f"stdout:\n{completed.stdout}\nstderr:\n{completed.stderr}"
                    )
                runs[executor] = {
                    "marker": parse_marker(completed.stdout, executor),
                    "runtime": parse_runtime(completed.stderr, executor),
                }
            pooled_marker = runs["pooled"]["marker"]
            dedicated_marker = runs["dedicated"]["marker"]
            assert isinstance(pooled_marker, dict)
            assert isinstance(dedicated_marker, dict)
            compared = [name for name in SEMANTIC_FIELDS if name != "executor"]
            mismatches = [
                name
                for name in compared
                if pooled_marker.get(name) != dedicated_marker.get(name)
            ]
            if mismatches:
                raise ExecutorDifferentialError(
                    "executor semantic mismatch: " + ", ".join(mismatches)
                )
        write_json(
            args.output.resolve(),
            {
                "schema_version": 1,
                "kind": "postgamma.executor-differential",
                "status": "pass",
                "compared_semantic_fields": compared,
                "runs": runs,
                "inputs": input_identity(inputs),
            },
        )
    except (OSError, LifecycleCheckError, ExecutorDifferentialError) as exc:
        parser.error(str(exc))
    print("executor differential evidence: pass (pooled == dedicated Oracle)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
