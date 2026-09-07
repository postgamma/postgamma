#!/usr/bin/env python3
"""Remove one explicitly bounded build directory and optionally recreate it."""

from __future__ import annotations

import argparse
import shutil
from pathlib import Path


def is_strict_child(path: Path, parent: Path) -> bool:
    try:
        path.relative_to(parent)
        return path != parent
    except ValueError:
        return False


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--path", required=True, type=Path)
    parser.add_argument("--within", required=True, type=Path)
    parser.add_argument("--remove-only", action="store_true")
    args = parser.parse_args()
    target = args.path.resolve()
    boundary = args.within.resolve()
    if not is_strict_child(target, boundary):
        parser.error(f"refusing to remove {target}: not a strict child of {boundary}")
    if target.exists():
        shutil.rmtree(target)
    if not args.remove_only:
        target.mkdir(parents=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
