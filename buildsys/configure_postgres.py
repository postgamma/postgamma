#!/usr/bin/env python3
"""Configure a PostgreSQL build tree from explicit, relocatable inputs."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import shlex
import shutil
import subprocess
from pathlib import Path
from typing import Mapping, Sequence

from reset_directory import is_strict_child


STATE_FILENAME = ".postgamma-configure.json"
CONFIGURE_ENVIRONMENT = (
    "AR",
    "AWK",
    "BISON",
    "CC",
    "CFLAGS",
    "CLANG",
    "CONFIG_SITE",
    "CPATH",
    "CPP",
    "CPPFLAGS",
    "CPLUS_INCLUDE_PATH",
    "C_INCLUDE_PATH",
    "CXX",
    "CXXFLAGS",
    "FLEX",
    "ICU_CFLAGS",
    "ICU_LIBS",
    "LANG",
    "LC_ALL",
    "LC_CTYPE",
    "LDFLAGS",
    "LDFLAGS_EX",
    "LDFLAGS_SL",
    "LD_LIBRARY_PATH",
    "LIBCURL_CFLAGS",
    "LIBCURL_LIBS",
    "LIBNUMA_CFLAGS",
    "LIBNUMA_LIBS",
    "LIBRARY_PATH",
    "LIBURING_CFLAGS",
    "LIBURING_LIBS",
    "LIBS",
    "LLVM_CONFIG",
    "LZ4_CFLAGS",
    "LZ4_LIBS",
    "MSGFMT",
    "PATH",
    # PostgreSQL's untouched upstream build still selects a Perl interpreter.
    "PERL",
    "PKG_CONFIG",
    "PKG_CONFIG_LIBDIR",
    "PKG_CONFIG_PATH",
    "PKG_CONFIG_SYSROOT_DIR",
    "PYTHON",
    "STRIP",
    "TAR",
    "TCLSH",
    "XML2_CONFIG",
    "XML2_CFLAGS",
    "XML2_LIBS",
    "ZSTD_CFLAGS",
    "ZSTD_LIBS",
)


class ConfigurationError(RuntimeError):
    """An unsafe or unsuccessful configure request."""


def arguments_from_environment(
    names: Sequence[str], environment: Mapping[str, str]
) -> list[str]:
    arguments: list[str] = []
    for name in names:
        value = environment.get(name, "")
        if not value:
            continue
        try:
            arguments.extend(shlex.split(value, posix=True))
        except ValueError as exc:
            raise ConfigurationError(f"cannot parse {name}: {exc}") from exc
    return arguments


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def configuration_request(
    source: Path,
    arguments: Sequence[str],
    environment: Mapping[str, str],
    inputs: Sequence[Path] = (),
) -> dict[str, object]:
    configure = source / "configure"
    request: dict[str, object] = {
        "schema_version": 1,
        "source": str(source),
        "configure_sha256": sha256(configure),
        "arguments": list(arguments),
        "environment": {
            name: environment[name]
            for name in CONFIGURE_ENVIRONMENT
            if name in environment
        },
    }
    if inputs:
        request["inputs"] = {
            str(path.resolve()): sha256(path.resolve()) for path in inputs
        }
    return request


def read_state(path: Path) -> dict[str, object] | None:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (FileNotFoundError, json.JSONDecodeError, OSError):
        return None
    return value if isinstance(value, dict) else None


def ensure_configured(
    source: Path,
    build: Path,
    build_root: Path,
    arguments: Sequence[str],
    environment: Mapping[str, str],
    invalidations: Sequence[Path] = (),
    inputs: Sequence[Path] = (),
) -> bool:
    source = source.resolve()
    build = build.resolve()
    build_root = build_root.resolve()
    configure = source / "configure"
    state_path = build / STATE_FILENAME

    if not configure.is_file():
        raise ConfigurationError(f"PostgreSQL configure script is missing: {configure}")
    if not is_strict_child(build, build_root):
        raise ConfigurationError(
            f"refusing to manage {build}: it is not a strict child of {build_root}"
        )
    if build == source or is_strict_child(source, build):
        raise ConfigurationError(f"refusing to replace a build tree containing {source}")

    managed_invalidations: list[Path] = []
    for value in invalidations:
        target = value.resolve()
        if not is_strict_child(target, build_root):
            raise ConfigurationError(
                f"refusing to invalidate {target}: it is not a strict child of {build_root}"
            )
        if target == build or is_strict_child(build, target):
            raise ConfigurationError(
                f"refusing redundant invalidation containing the build tree: {target}"
            )
        if target == source or is_strict_child(source, target):
            raise ConfigurationError(
                f"refusing invalidation containing the source tree: {target}"
            )
        managed_invalidations.append(target)

    missing_inputs = [path for path in inputs if not path.is_file()]
    if missing_inputs:
        raise ConfigurationError(
            "configuration input(s) missing: "
            + ", ".join(str(path) for path in missing_inputs)
        )
    request = configuration_request(source, arguments, environment, inputs)
    if (build / "config.status").is_file() and read_state(state_path) == request:
        print(f"configure: up to date ({build})", flush=True)
        return False

    if build.exists():
        shutil.rmtree(build)
    for target in managed_invalidations:
        if target.is_dir():
            shutil.rmtree(target)
            print(f"configure: invalidated {target}", flush=True)
        elif target.exists():
            target.unlink()
            print(f"configure: invalidated {target}", flush=True)
    build.mkdir(parents=True)
    command = [str(configure), *arguments]
    print(f"configure: {shlex.join(command)}", flush=True)
    try:
        result = subprocess.run(command, cwd=build, env=dict(environment), check=False)
    except OSError as exc:
        raise ConfigurationError(f"cannot execute PostgreSQL configure: {exc}") from exc
    if result.returncode:
        raise ConfigurationError(
            f"PostgreSQL configure failed with exit status {result.returncode}"
        )
    if not (build / "config.status").is_file():
        raise ConfigurationError("PostgreSQL configure did not create config.status")
    state_path.write_text(
        json.dumps(request, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    return True


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", required=True, type=Path)
    parser.add_argument("--build", required=True, type=Path)
    parser.add_argument("--build-root", required=True, type=Path)
    parser.add_argument(
        "--input",
        action="append",
        default=[],
        type=Path,
        metavar="PATH",
        help="hash an input into the configuration identity",
    )
    parser.add_argument(
        "--invalidate",
        action="append",
        default=[],
        type=Path,
        metavar="PATH",
        help="remove a dependent path inside build root when configure inputs change",
    )
    parser.add_argument(
        "--args-env",
        action="append",
        default=[],
        metavar="NAME",
        help="append shell-style arguments from environment variable NAME",
    )
    args = parser.parse_args()
    try:
        configure_arguments = arguments_from_environment(args.args_env, os.environ)
        ensure_configured(
            args.source,
            args.build,
            args.build_root,
            configure_arguments,
            os.environ,
            args.invalidate,
            args.input,
        )
    except ConfigurationError as exc:
        parser.error(str(exc))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
