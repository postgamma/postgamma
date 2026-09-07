#!/usr/bin/env python3
"""Shared deterministic discovery for the local LLVM toolchain."""

from __future__ import annotations

import os
import re
import shutil
import subprocess
from pathlib import Path


# Keep newest validated releases first so an explicitly versioned installation
# wins over a moving, unversioned distro default.  LLVM 21 is exercised by the
# pinned manylinux_2_28 release image, while LLVM 22 is exercised on Fedora 44;
# both also pass the normal AST compile/link/run probe.
LLVM_MAJORS = (22, 21, 20, 19, 18, 17, 16)


def _executable(candidate: str | Path) -> str | None:
    path = Path(candidate)
    if path.is_absolute() or "/" in str(candidate):
        # Keep the spelling of driver symlinks such as clang++.  Resolving the
        # link to clang changes argv[0], which makes Clang select the C driver
        # and silently drops the C++ runtime from the final link command.
        return str(path.absolute()) if path.is_file() and os.access(path, os.X_OK) else None
    return shutil.which(str(candidate))


def find_llvm_config(root: Path) -> str | None:
    candidates: list[str | Path] = []
    if value := os.environ.get("LLVM_CONFIG"):
        candidates.append(value)
    candidates.append(root / ".deps" / "llvm" / "bin" / "llvm-config")
    for major in LLVM_MAJORS:
        candidates.extend((f"llvm-config-{major}", f"/usr/lib/llvm-{major}/bin/llvm-config"))
    # Prefer an explicitly supported version over an unversioned system
    # default, which may move to a newer, not-yet-supported LLVM release.
    candidates.append("llvm-config")
    for candidate in candidates:
        if found := _executable(candidate):
            return found
    return None


def find_clangxx(root: Path, llvm_config: str | None = None) -> str | None:
    candidates: list[str | Path] = []
    if value := os.environ.get("CLANGXX"):
        candidates.append(value)
    candidates.append(root / ".deps" / "llvm" / "bin" / "clang++")
    if llvm_config:
        candidates.append(Path(llvm_config).resolve().parent / "clang++")
    for major in LLVM_MAJORS:
        candidates.extend((f"clang++-{major}", f"/usr/lib/llvm-{major}/bin/clang++"))
    # As with llvm-config, an unversioned distro default is the final fallback.
    candidates.append("clang++")
    for candidate in candidates:
        if found := _executable(candidate):
            return found
    return None


def _tool_query(command: str, argument: str) -> str | None:
    try:
        result = subprocess.run(
            [command, argument],
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            text=True,
            timeout=10,
        )
    except (OSError, subprocess.TimeoutExpired):
        return None
    value = result.stdout.strip()
    return value if result.returncode == 0 and value else None


def find_clang_cpp_library(llvm_config: str | None) -> str | None:
    """Return the matching shared libclang-cpp path across distro layouts."""
    if llvm_config is None:
        return None
    libdir_value = _tool_query(llvm_config, "--libdir")
    version = _tool_query(llvm_config, "--version")
    if libdir_value is None:
        return None
    libdir = Path(libdir_value)
    candidates = [libdir / "libclang-cpp.so"]
    match = re.match(r"([0-9]+)", version or "")
    if match:
        candidates.extend(sorted(libdir.glob(f"libclang-cpp.so.{match.group(1)}*")))
    for candidate in candidates:
        if candidate.is_file():
            return str(candidate.absolute())
    return None


def find_clang_resource_dir(clangxx: str | None) -> str | None:
    """Return Clang's own builtin-header directory when it exists."""
    if clangxx is None:
        return None
    value = _tool_query(clangxx, "--print-resource-dir")
    if value is None:
        return None
    path = Path(value)
    return str(path.resolve()) if path.is_dir() else None
