#!/usr/bin/env python3
"""Compile and run the public quickstart against a staged SDK."""

from __future__ import annotations

import argparse
import hashlib
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
from check_arrow_management import parse_runtime, prepend_library_path


MARKER = "POSTGAMMA_QUICKSTART"
MARKER_VALUES = {
    "abi": "65539",
    "capabilities": "112639",
    "postgres": "19",
    "rows": "1",
    "name": "planner",
    "score": "98.5",
    "checkpoint": "true",
    "phase": "closed",
}


class InstalledSdkCheckError(RuntimeError):
    """The staged public SDK did not build or behave as documented."""


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def marker_fields(stdout: str) -> dict[str, str]:
    lines = [line for line in stdout.splitlines() if line.startswith(f"{MARKER} ")]
    if len(lines) != 1:
        raise InstalledSdkCheckError(
            f"expected one {MARKER} line, found {len(lines)}"
        )
    values = dict(re.findall(r"([a-z_]+)=([^\s]+)", lines[0]))
    for name, expected in MARKER_VALUES.items():
        if values.get(name) != expected:
            raise InstalledSdkCheckError(
                f"quickstart marker field {name} is not {expected}"
            )
    return values


def stage_file(
    source: Path, destination: Path, sdk_root: Path
) -> dict[str, str]:
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, destination)
    if sha256(source) != sha256(destination):
        raise InstalledSdkCheckError(f"staged file differs from {source}")
    return {
        "path": str(destination.relative_to(sdk_root)),
        "sha256": sha256(destination),
    }


def require_success(name: str, completed: Any) -> None:
    if completed.returncode != 0:
        raise InstalledSdkCheckError(
            f"{name} failed ({completed.returncode})\n"
            f"stdout:\n{completed.stdout}\nstderr:\n{completed.stderr}"
        )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--make", default="make")
    parser.add_argument("--jobs", type=int, default=1)
    parser.add_argument("--cc", default="cc")
    parser.add_argument("--cxx", default="c++")
    parser.add_argument("--readelf", default="readelf")
    parser.add_argument("--strace", default="strace")
    parser.add_argument("--postgres-build", required=True, type=Path)
    parser.add_argument("--library", required=True, type=Path)
    parser.add_argument("--core-header", required=True, type=Path)
    parser.add_argument("--arrow-header", required=True, type=Path)
    parser.add_argument("--extension-header", required=True, type=Path)
    parser.add_argument("--extension-example", required=True, type=Path)
    parser.add_argument("--example", required=True, type=Path)
    parser.add_argument("--work-root", required=True, type=Path)
    parser.add_argument("--stdout", required=True, type=Path)
    parser.add_argument("--stderr", required=True, type=Path)
    parser.add_argument("--trace", required=True, type=Path)
    parser.add_argument("--input", action="append", default=[], type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()

    generated = (args.stdout, args.stderr, args.trace, args.output)
    for path in generated:
        path.resolve().parent.mkdir(parents=True, exist_ok=True)
        path.resolve().unlink(missing_ok=True)
    try:
        postgres_build = args.postgres_build.resolve(strict=True)
        library = args.library.resolve(strict=True)
        core_header = args.core_header.resolve(strict=True)
        arrow_header = args.arrow_header.resolve(strict=True)
        extension_header = args.extension_header.resolve(strict=True)
        extension_example = args.extension_example.resolve(strict=True)
        example = args.example.resolve(strict=True)
        inputs = [path.resolve(strict=True) for path in args.input]
        work_root = args.work_root.resolve()
        work_root.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(
            prefix="api_release-installed-sdk-", dir=work_root
        ) as temporary:
            temporary_path = Path(temporary)
            postgres_install = temporary_path / "postgres-install"
            run_checked(
                [
                    args.make,
                    "-C",
                    str(postgres_build),
                    f"-j{args.jobs}",
                    "install",
                    f"DESTDIR={postgres_install}",
                ],
                timeout=300.0,
            )
            postgres_prefix = installed_prefix(postgres_install)
            sdk = temporary_path / "sdk"
            staged = [
                stage_file(
                    core_header,
                    sdk / "include" / "postgamma" / "postgamma.h",
                    sdk,
                ),
                stage_file(
                    arrow_header,
                    sdk / "include" / "postgamma" / "postgamma_arrow.h",
                    sdk,
                ),
                stage_file(
                    extension_header,
                    sdk / "include" / "postgamma" / "postgamma_extension.h",
                    sdk,
                ),
                stage_file(library, sdk / "lib" / "libpostgamma.so", sdk),
            ]
            extension_objects: list[str] = []
            for compiler, language, standard in (
                (args.cc, "c", "c11"),
                (args.cxx, "c++", "c++17"),
            ):
                extension_object = sdk / "lib" / f"extension-sdk-{language}.o"
                run_checked(
                    [
                        compiler,
                        "-x",
                        language,
                        f"-std={standard}",
                        "-Wall",
                        "-Wextra",
                        "-Werror",
                        "-Wpedantic",
                        f"-I{sdk / 'include'}",
                        "-c",
                        str(extension_example),
                        "-o",
                        str(extension_object),
                    ],
                    timeout=60.0,
                )
                extension_objects.append(language)
            executable = sdk / "bin" / "postgamma-quickstart"
            executable.parent.mkdir(parents=True, exist_ok=True)
            compile_command = [
                args.cc,
                "-std=c11",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-Wpedantic",
                f"-I{sdk / 'include'}",
                str(example),
                f"-L{sdk / 'lib'}",
                "-lpostgamma",
                "-pthread",
                "-Wl,-z,defs",
                "-Wl,-rpath,$ORIGIN/../lib",
                "-o",
                str(executable),
            ]
            run_checked(compile_command, timeout=60.0)
            dynamic = run_checked(
                [args.readelf, "-d", str(executable)], timeout=30.0
            ).stdout
            needed = re.findall(r"Shared library: \[([^]]+)\]", dynamic)
            if "libpostgamma.so" not in needed:
                raise InstalledSdkCheckError(
                    "installed consumer does not link libpostgamma.so"
                )
            environment = runtime_environment(postgres_prefix)
            prepend_library_path(environment, sdk / "lib")
            environment["LC_ALL"] = "C"
            command = [
                str(executable),
                str(temporary_path / "data"),
                str(postgres_prefix / "bin" / "postgres"),
                str(postgres_prefix),
                "create",
            ]
            normal = run_checked(
                command, environment=environment, timeout=300.0, check=False
            )
            traced_command = [
                args.strace,
                "-f",
                "-qq",
                "-o",
                str(args.trace.resolve()),
                "-e",
                "trace=process,signal,network,chdir,fchdir,umask,setitimer",
                str(executable),
                str(temporary_path / "trace-data"),
                str(postgres_prefix / "bin" / "postgres"),
                str(postgres_prefix),
                "create",
            ]
            traced = run_checked(
                traced_command,
                environment=environment,
                timeout=300.0,
                check=False,
            )
            require_success("installed quickstart", normal)
            require_success("traced installed quickstart", traced)
            marker = marker_fields(normal.stdout)
            trace_marker = marker_fields(traced.stdout)
            runtime = parse_runtime(normal.stderr, 2)
            trace_runtime = parse_runtime(traced.stderr, 2)
            host_safety = audit_trace(
                args.trace.resolve().read_text(encoding="utf-8"), executable
            )
            args.stdout.resolve().write_text(normal.stdout, encoding="utf-8")
            args.stderr.resolve().write_text(normal.stderr, encoding="utf-8")
            document = {
                "schema_version": 1,
                "kind": "postgamma.installed-c-sdk",
                "status": "pass",
                "postgresql_major": 19,
                "marker": marker,
                "trace_marker": trace_marker,
                "runtime": runtime,
                "trace_runtime": trace_runtime,
                "host_safety": host_safety,
                "consumer": {
                    "source": str(example),
                    "source_include_directories": ["include"],
                    "repository_include_directories": 0,
                    "linked_library": "libpostgamma.so",
                    "dynamic_dependencies": needed,
                },
                "staged_sdk": staged,
                "extension_sdk": {
                    "source": str(extension_example),
                    "compiled_languages": extension_objects,
                    "postgresql_headers_required": 0,
                },
                "inputs": input_identity(
                    [
                        *inputs,
                        library,
                        core_header,
                        arrow_header,
                        extension_header,
                        extension_example,
                        example,
                    ]
                ),
            }
            write_json(args.output.resolve(), document)
    except (
        InstalledSdkCheckError,
        LifecycleCheckError,
        OSError,
        PublicApiCheckError,
    ) as exc:
        parser.error(str(exc))
    print(
        "installed C SDK evidence: pass "
        "(standalone header consumer, linked shared library, native checkpoint)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
