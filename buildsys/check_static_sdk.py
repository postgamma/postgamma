#!/usr/bin/env python3
"""Compile and execute the public quickstart through the staged static SDK."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shlex
import shutil
import tempfile
from pathlib import Path
from typing import Any, Sequence

from check_embedded_lifecycle import (
    LifecycleCheckError,
    input_identity,
    run_checked,
    runtime_environment,
    write_json,
)
from check_embedded_public_api import PublicApiCheckError, audit_trace
from check_arrow_management import parse_runtime
from check_installed_c_sdk import (
    InstalledSdkCheckError,
    marker_fields,
    require_success,
    stage_file,
)


EVIDENCE_KIND = "postgamma.static-sdk-evidence"
RECEIPT_KIND = "postgamma.static-library-build"
NEEDED = re.compile(r"Shared library: \[([^]]+)\]")
PGVECTOR_MARKER = "POSTGAMMA_STATIC_PGVECTOR"
PGVECTOR_MARKER_VALUES = {
    "version": "0.8.6",
    "hnsw": "true",
    "nearest": "1",
    "phase": "closed",
}


class StaticSdkCheckError(RuntimeError):
    """The final static SDK is not isolated, consumable, or executable."""


def load_json(path: Path) -> dict[str, Any]:
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise StaticSdkCheckError(f"cannot read {path}: {error}") from error
    if not isinstance(document, dict):
        raise StaticSdkCheckError(f"{path}: top-level value must be an object")
    return document


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def global_definitions(nm: str, path: Path) -> set[str]:
    output = run_checked(
        [
            nm,
            "--defined-only",
            "--extern-only",
            "--format=posix",
            str(path),
        ],
        timeout=60.0,
    ).stdout
    symbols: set[str] = set()
    for line in output.splitlines():
        fields = line.split()
        if fields and not fields[0].endswith(":"):
            symbols.add(fields[0])
    return symbols


def pkg_config_environment(sdk: Path) -> dict[str, str]:
    environment = dict(os.environ)
    environment["PKG_CONFIG_PATH"] = str(sdk / "lib/pkgconfig")
    environment.pop("PKG_CONFIG_LIBDIR", None)
    return environment


def selects_static_archive(flags: Sequence[str], archive: Path) -> bool:
    """Return whether pkg-config selected the requested archive by path."""

    expected = archive.resolve()
    return any(
        not flag.startswith("-") and Path(flag).resolve() == expected
        for flag in flags
    )


def pkg_config_flags(executable: str, sdk: Path) -> list[str]:
    environment = pkg_config_environment(sdk)
    output = run_checked(
        [executable, "--cflags", "--static", "--libs", "postgamma"],
        environment=environment,
        timeout=30.0,
    ).stdout
    try:
        flags = shlex.split(output)
    except ValueError as error:
        raise StaticSdkCheckError(
            f"pkg-config returned invalid compiler flags: {error}"
        ) from error
    if not flags:
        raise StaticSdkCheckError("pkg-config returned no static SDK flags")
    archive = sdk / "lib/libpostgamma.a"
    if not selects_static_archive(flags, archive) or "-lpostgamma" in flags:
        raise StaticSdkCheckError(
            "pkg-config does not select the PostGamma static archive explicitly"
        )
    return flags


def pgvector_marker_fields(stdout: str) -> dict[str, str]:
    lines = [
        line for line in stdout.splitlines() if line.startswith(PGVECTOR_MARKER + " ")
    ]
    if len(lines) != 1:
        raise StaticSdkCheckError(
            f"expected one {PGVECTOR_MARKER} line, found {len(lines)}"
        )
    values = dict(re.findall(r"([a-z_]+)=([^\s]+)", lines[0]))
    if values != PGVECTOR_MARKER_VALUES:
        raise StaticSdkCheckError("static pgvector marker changed")
    return values


def run_quickstart(
    *,
    executable: Path,
    resource_root: Path,
    data_directory: Path,
    environment: dict[str, str],
    timeout: float = 300.0,
    prefix: Sequence[str] = (),
) -> Any:
    return run_checked(
        [
            *prefix,
            str(executable),
            str(data_directory),
            str(resource_root / "bin/postgres"),
            str(resource_root),
            "create",
        ],
        environment=environment,
        timeout=timeout,
        check=False,
    )


def check(args: argparse.Namespace) -> dict[str, Any]:
    library = args.library.resolve(strict=True)
    link_options = args.link_options.resolve(strict=True)
    pkg_config = args.pkg_config_file.resolve(strict=True)
    receipt_path = args.receipt.resolve(strict=True)
    core_header = args.core_header.resolve(strict=True)
    arrow_header = args.arrow_header.resolve(strict=True)
    extension_header = args.extension_header.resolve(strict=True)
    example = args.example.resolve(strict=True)
    pgvector_example = args.pgvector_example.resolve(strict=True)
    resource_root = args.resource_root.resolve(strict=True)
    inputs = [path.resolve(strict=True) for path in args.input]
    if not (resource_root / "bin/postgres").is_file() or not (
        resource_root / "share/postgres.bki"
    ).is_file():
        raise StaticSdkCheckError(f"invalid resource root: {resource_root}")

    receipt = load_json(receipt_path)
    if (
        receipt.get("schema_version") != 1
        or receipt.get("kind") != RECEIPT_KIND
        or receipt.get("status") != "pass"
        or receipt.get("linkage") != "static"
        or receipt.get("version") != "1.3"
        or receipt.get("output_sha256") != sha256(library)
        or receipt.get("determinism")
        != {"build_count": 2, "identical": True}
    ):
        raise StaticSdkCheckError("static-library receipt is invalid or stale")
    public_symbols = receipt.get("public_symbols")
    if (
        not isinstance(public_symbols, list)
        or len(public_symbols) != 75
        or not all(isinstance(symbol, str) for symbol in public_symbols)
        or set(public_symbols) != global_definitions(args.nm, library)
    ):
        raise StaticSdkCheckError(
            "static archive global definitions differ from the public C ABI"
        )
    declared_options = receipt.get("consumer_link_options")
    if (
        not isinstance(declared_options, list)
        or not all(isinstance(option, str) for option in declared_options)
        or shlex.split(link_options.read_text(encoding="utf-8"))
        != declared_options
    ):
        raise StaticSdkCheckError("static consumer link options are stale")
    pkg_config_text = pkg_config.read_text(encoding="utf-8")
    if str(library.parent.parent) in pkg_config_text:
        raise StaticSdkCheckError("pkg-config metadata captured its build directory")
    if "URL: https://postgamma.com\n" not in pkg_config_text:
        raise StaticSdkCheckError("pkg-config metadata has no official project URL")
    sdk_root = library.parent.parent
    expected_headers = [
        {
            "path": str(path.relative_to(sdk_root)),
            "sha256": sha256(path),
        }
        for path in (core_header, arrow_header, extension_header)
    ]
    if receipt.get("headers") != expected_headers:
        raise StaticSdkCheckError("static SDK public headers are missing or stale")

    work_root = args.work_root.resolve()
    work_root.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="static-sdk-", dir=work_root) as temporary:
        root = Path(temporary)
        sdk = root / "sdk"
        staged = [
            stage_file(
                core_header,
                sdk / "include/postgamma/postgamma.h",
                sdk,
            ),
            stage_file(
                arrow_header,
                sdk / "include/postgamma/postgamma_arrow.h",
                sdk,
            ),
            stage_file(
                extension_header,
                sdk / "include/postgamma/postgamma_extension.h",
                sdk,
            ),
            stage_file(library, sdk / "lib/libpostgamma.a", sdk),
            stage_file(
                link_options,
                sdk / "lib/postgamma-static-libs.txt",
                sdk,
            ),
            stage_file(
                pkg_config,
                sdk / "lib/pkgconfig/postgamma.pc",
                sdk,
            ),
        ]
        executable = sdk / "bin/postgamma-static-quickstart"
        pgvector_executable = sdk / "bin/postgamma-static-pgvector"
        executable.parent.mkdir(parents=True)
        flags = pkg_config_flags(args.pkg_config, sdk)
        pkg_version = run_checked(
            [args.pkg_config, "--modversion", "postgamma"],
            environment=pkg_config_environment(sdk),
            timeout=30.0,
        ).stdout.strip()
        if pkg_version != receipt["version"]:
            raise StaticSdkCheckError("pkg-config version differs from the static SDK")
        run_checked(
            [
                args.cc,
                "-std=c11",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-Wpedantic",
                str(example),
                *flags,
                "-Wl,-z,defs",
                "-o",
                str(executable),
            ],
            timeout=120.0,
        )
        run_checked(
            [
                args.cc,
                "-std=c11",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-Wpedantic",
                str(pgvector_example),
                *flags,
                "-Wl,-z,defs",
                "-o",
                str(pgvector_executable),
            ],
            timeout=120.0,
        )
        dynamic = run_checked(
            [args.readelf, "-d", str(executable)], timeout=30.0
        ).stdout
        dependencies = sorted(NEEDED.findall(dynamic))
        if any(name.startswith("libpostgamma") for name in dependencies):
            raise StaticSdkCheckError(
                "static consumer retains a PostGamma shared-library dependency"
            )
        if "RUNPATH" in dynamic or "RPATH" in dynamic:
            raise StaticSdkCheckError("static consumer contains a runtime library path")
        pgvector_dynamic = run_checked(
            [args.readelf, "-d", str(pgvector_executable)], timeout=30.0
        ).stdout
        pgvector_dependencies = sorted(NEEDED.findall(pgvector_dynamic))
        if any(name.startswith("libpostgamma") for name in pgvector_dependencies):
            raise StaticSdkCheckError(
                "static pgvector consumer retains a PostGamma shared-library dependency"
            )
        if "RUNPATH" in pgvector_dynamic or "RPATH" in pgvector_dynamic:
            raise StaticSdkCheckError(
                "static pgvector consumer contains a runtime library path"
            )

        environment = runtime_environment(resource_root)
        environment["LC_ALL"] = "C"
        normal = run_quickstart(
            executable=executable,
            resource_root=resource_root,
            data_directory=root / "data",
            environment=environment,
        )
        trace_path = args.trace.resolve()
        traced = run_quickstart(
            executable=executable,
            resource_root=resource_root,
            data_directory=root / "trace-data",
            environment=environment,
            prefix=(
                args.strace,
                "-f",
                "-qq",
                "-o",
                str(trace_path),
                "-e",
                "trace=process,signal,network,chdir,fchdir,umask,setitimer",
            ),
        )
        pgvector = run_checked(
            [
                str(pgvector_executable),
                str(root / "pgvector-data"),
                str(resource_root / "bin/postgres"),
                str(resource_root),
            ],
            environment=environment,
            timeout=300.0,
            check=False,
        )
        require_success("static SDK quickstart", normal)
        require_success("traced static SDK quickstart", traced)
        require_success("static SDK pgvector consumer", pgvector)
        marker = marker_fields(normal.stdout)
        trace_marker = marker_fields(traced.stdout)
        pgvector_marker = pgvector_marker_fields(pgvector.stdout)
        runtime = parse_runtime(normal.stderr, 2)
        trace_runtime = parse_runtime(traced.stderr, 2)
        host_safety = audit_trace(
            trace_path.read_text(encoding="utf-8"), executable
        )
        args.stdout.resolve().write_text(normal.stdout, encoding="utf-8")
        args.stderr.resolve().write_text(normal.stderr, encoding="utf-8")

    return {
        "schema_version": 1,
        "kind": EVIDENCE_KIND,
        "status": "pass",
        "postgresql_major": 19,
        "linkage": "static",
        "sdk_version": receipt["version"],
        "archive": {
            "name": library.name,
            "sha256": sha256(library),
            "public_symbol_count": len(public_symbols),
            "localized_internal_symbol_count": receipt.get(
                "localized_internal_symbol_count"
            ),
            "exposed_internal_symbol_count": 0,
            "deterministic": True,
        },
        "consumer": {
            "source": str(example),
            "pkg_config": "postgamma",
            "dynamic_dependencies": dependencies,
            "postgamma_shared_dependencies": 0,
            "runtime_paths": 0,
        },
        "pgvector_consumer": {
            "source": str(pgvector_example),
            "marker": pgvector_marker,
            "dynamic_dependencies": pgvector_dependencies,
            "postgamma_shared_dependencies": 0,
            "runtime_paths": 0,
        },
        "marker": marker,
        "trace_marker": trace_marker,
        "runtime": runtime,
        "trace_runtime": trace_runtime,
        "host_safety": host_safety,
        "staged_sdk": staged,
        "inputs": input_identity(
            [
                *inputs,
                library,
                link_options,
                pkg_config,
                receipt_path,
                core_header,
                arrow_header,
                extension_header,
                example,
                pgvector_example,
            ]
        ),
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--cc", default="cc")
    parser.add_argument("--nm", default="nm")
    parser.add_argument("--readelf", default="readelf")
    parser.add_argument("--strace", default="strace")
    parser.add_argument("--pkg-config", default="pkg-config")
    parser.add_argument("--library", required=True, type=Path)
    parser.add_argument("--link-options", required=True, type=Path)
    parser.add_argument("--pkg-config-file", required=True, type=Path)
    parser.add_argument("--receipt", required=True, type=Path)
    parser.add_argument("--core-header", required=True, type=Path)
    parser.add_argument("--arrow-header", required=True, type=Path)
    parser.add_argument("--extension-header", required=True, type=Path)
    parser.add_argument("--example", required=True, type=Path)
    parser.add_argument("--pgvector-example", required=True, type=Path)
    parser.add_argument("--resource-root", required=True, type=Path)
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
        document = check(args)
        write_json(args.output.resolve(), document)
    except (
        InstalledSdkCheckError,
        LifecycleCheckError,
        OSError,
        PublicApiCheckError,
        StaticSdkCheckError,
    ) as error:
        parser.error(str(error))
    print(
        "PostGamma static SDK evidence: pass "
        "(relocatable pkg-config consumers, bundled pgvector, no PostGamma DSO dependency)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
