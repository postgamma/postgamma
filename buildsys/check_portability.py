#!/usr/bin/env python3
"""Check project-owned source files for build-host path dependencies."""

from __future__ import annotations

import argparse
import json
import os
import re
from pathlib import Path
from typing import Iterable


POSIX_WORKSPACE = re.compile(
    rb"/(?:home|Users)/[^/\s\"']+/(?:codes|projects|src|workspaces?)/"
)
WINDOWS_WORKSPACE = re.compile(
    rb"[A-Za-z]:\\Users\\[^\\\s\"']+\\(?:codes|projects|src|workspaces?)\\",
    re.IGNORECASE,
)
EXCLUDED_ROOT_DIRECTORIES = {
    ".deps",
    ".git",
    "build",
    "postgres",
    "third_party",
}
EXCLUDED_DIRECTORY_NAMES = {"__pycache__"}
PATH_CONTINUATION_BYTES = frozenset(
    b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_./\\-$)}]"
)


def project_files(root: Path) -> list[Path]:
    """Return all project-owned files, including untracked source files."""
    paths: list[Path] = []
    for directory, names, filenames in os.walk(root, topdown=True):
        current = Path(directory)
        if current == root:
            names[:] = [
                name for name in names if name not in EXCLUDED_ROOT_DIRECTORIES
            ]
        names[:] = [name for name in names if name not in EXCLUDED_DIRECTORY_NAMES]
        paths.extend(
            current / filename
            for filename in filenames
            if not (current / filename).is_symlink()
        )
    return sorted(paths)


def forbidden_offsets(data: bytes, explicit_prefixes: Iterable[bytes]) -> list[int]:
    offsets: set[int] = set()
    for prefix in explicit_prefixes:
        start = 0
        while prefix and (offset := data.find(prefix, start)) >= 0:
            # A short checkout such as /src must not make a relative suffix in
            # $(BUILD_DIR)/src/include look like a leaked absolute workspace.
            # Only accept the explicit prefix at the start of a path token.
            if offset == 0 or data[offset - 1] not in PATH_CONTINUATION_BYTES:
                offsets.add(offset)
            start = offset + 1
    for pattern in (POSIX_WORKSPACE, WINDOWS_WORKSPACE):
        offsets.update(match.start() for match in pattern.finditer(data))
    return sorted(offsets)


def directory_prefix(path: Path) -> bytes:
    """Return an absolute directory prefix with a component boundary."""
    value = os.fsencode(path.resolve())
    separator = os.fsencode(os.sep)
    return value if value.endswith(separator) else value + separator


def developer_directory_prefixes(paths: Iterable[Path]) -> tuple[bytes, ...]:
    """Return only directory prefixes specific enough to identify a workspace."""
    prefixes: list[bytes] = []
    for path in paths:
        resolved = path.resolve()
        # Container checkouts commonly live directly below / (for example
        # /src).  Such names are generic path components rather than developer
        # identities and collide with ordinary source-tree suffixes.
        if len(resolved.parts) >= 3:
            prefix = directory_prefix(resolved)
            if prefix not in prefixes:
                prefixes.append(prefix)
    return tuple(prefixes)


def line_number(data: bytes, offset: int) -> int:
    return data.count(b"\n", 0, offset) + 1


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", required=True, type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    root = args.root.resolve()
    prefixes = developer_directory_prefixes((root, Path.home()))
    failures: list[str] = []
    checked_file_count = 0
    for path in project_files(root):
        if not path.is_file():
            continue
        checked_file_count += 1
        data = path.read_bytes()
        if b"\0" in data:
            continue
        for offset in forbidden_offsets(data, prefixes):
            failures.append(
                f"{path.relative_to(root)}:{line_number(data, offset)}: "
                "developer-specific absolute path"
            )
    if failures:
        parser.error(
            "portability violation(s) found in project source:\n  "
            + "\n  ".join(failures)
        )
    if args.output is not None:
        output = args.output.resolve()
        output.parent.mkdir(parents=True, exist_ok=True)
        document = {
            "schema_version": 1,
            "kind": "postgamma.portability-evidence",
            "status": "pass",
            "checked_file_count": checked_file_count,
            "developer_path_violations": 0,
        }
        output.write_text(
            json.dumps(document, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
    print("portability: pass")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
