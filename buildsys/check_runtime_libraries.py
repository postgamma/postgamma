#!/usr/bin/env python3
"""Inspect the host loader's dependency resolution without build-library overrides."""

from __future__ import annotations

import os
import re
import subprocess
from pathlib import Path
from typing import Iterable


class RuntimeLibraryError(RuntimeError):
    """An artifact needs a missing or build-only runtime library."""


def clean_environment() -> dict[str, str]:
    environment = dict(os.environ)
    for key in (
        "LD_LIBRARY_PATH", "LD_PRELOAD", "LD_AUDIT", "LD_DEBUG", "LD_DEBUG_OUTPUT",
        "LD_RUN_PATH", "LIBRARY_PATH", "CPATH", "GCC_EXEC_PREFIX", "COMPILER_PATH",
        "C_INCLUDE_PATH", "CPLUS_INCLUDE_PATH", "PKG_CONFIG_PATH",
        "PKG_CONFIG_LIBDIR", "PKG_CONFIG_SYSROOT_DIR", "CPPFLAGS", "LDFLAGS",
        "PYTHONPATH", "PYTHONHOME", "CONDA_PREFIX", "CONDA_DEFAULT_ENV",
    ):
        environment.pop(key, None)
    environment.update(LC_ALL="C", PATH="/usr/bin:/bin", PYTHONNOUSERSITE="1")
    return environment


def inspect_libraries(
    binary: Path,
    *,
    readelf: str = "readelf",
    forbidden: Iterable[Path] = (),
    maximum_glibc: tuple[int, ...] | None = None,
) -> dict[str, object]:
    binary = binary.resolve(strict=True)
    environment = clean_environment()
    dynamic = subprocess.run(
        [readelf, "--wide", "-d", "-V", str(binary)],
        env=environment, capture_output=True, text=True, check=True, timeout=30,
    ).stdout
    needed = sorted(set(re.findall(r"Shared library: \[([^]]+)\]", dynamic)))
    versions = sorted({
        tuple(map(int, value.split(".")))
        for value in re.findall(r"\bGLIBC_([0-9]+(?:\.[0-9]+)+)\b", dynamic)
    })
    if maximum_glibc and versions and versions[-1] > maximum_glibc:
        raise RuntimeLibraryError(f"{binary}: GLIBC_{'.'.join(map(str, versions[-1]))} exceeds the requested baseline")
    paths: dict[str, str] = {}
    if needed:
        result = subprocess.run(
            ["/usr/bin/ldd", str(binary)], env=environment,
            capture_output=True, text=True, check=False, timeout=30,
        )
        if result.returncode or "not found" in result.stdout + result.stderr:
            raise RuntimeLibraryError(f"{binary}: loader cannot resolve dependencies:\n{result.stdout}{result.stderr}")
        for line in result.stdout.splitlines():
            match = re.match(r"\s*(\S+) => (/.+?) \(0x[0-9a-fA-F]+\)", line)
            if match:
                paths[match[1]] = str(Path(match[2]).resolve(strict=True))
        if not set(needed).issubset(paths):
            # glibc may list the interpreter by its absolute name directly.
            for line in result.stdout.splitlines():
                match = re.match(r"\s*(/\S+) \(0x[0-9a-fA-F]+\)", line)
                if match:
                    paths[Path(match[1]).name] = str(Path(match[1]).resolve(strict=True))
        if missing := set(needed) - paths.keys():
            raise RuntimeLibraryError(f"{binary}: incomplete loader evidence for {sorted(missing)}")
    prefixes = [path.resolve() for path in forbidden]
    for name, path in paths.items():
        if any(Path(path).is_relative_to(prefix) for prefix in prefixes):
            raise RuntimeLibraryError(f"{binary}: {name} resolves inside build tools: {path}")
    return {
        "needed": needed,
        "resolved": paths,
        "glibc_required": ".".join(map(str, versions[-1])) if versions else None,
    }


def verify_loaded_libraries(
    objects: Iterable[str], expected: dict[str, str], *, readelf: str = "readelf",
) -> dict[str, str]:
    """Check actual loaded objects against standalone loader resolution.

    An interpreter may have already loaded another library with the same SONAME.
    Only product dependencies are compared; unrelated interpreter libraries do
    not become product requirements.
    """
    loaded: dict[str, str] = {}
    for value in sorted(set(objects)):
        path = Path(value).resolve(strict=True)
        dynamic = subprocess.run(
            [readelf, "--wide", "-d", str(path)], env=clean_environment(),
            capture_output=True, text=True, check=True, timeout=30,
        ).stdout
        soname = re.search(r"Library soname: \[([^]]+)\]", dynamic)
        if soname is None or soname[1] not in expected:
            continue
        name = soname[1]
        if path != Path(expected[name]).resolve(strict=True):
            raise RuntimeLibraryError(
                f"{name}: process loaded {path}, expected {expected[name]}"
            )
        loaded[name] = str(path)
    if missing := expected.keys() - loaded.keys():
        raise RuntimeLibraryError(f"process did not report product dependencies: {sorted(missing)}")
    return loaded
