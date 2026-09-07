#!/usr/bin/env python3
"""Materialize a pristine, committed source tree under build/."""

from __future__ import annotations

import argparse
import json
import shutil
import subprocess
import tarfile
import tempfile
from pathlib import Path


def strict_child(path: Path, parent: Path) -> bool:
    try:
        path.relative_to(parent)
        return path != parent
    except ValueError:
        return False


def git(repository: Path, *arguments: str) -> str:
    result = subprocess.run(
        ["git", "-C", str(repository), *arguments],
        check=True,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    return result.stdout.strip()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repository", required=True, type=Path)
    parser.add_argument("--manifest", required=True, type=Path)
    parser.add_argument("--destination", required=True, type=Path)
    parser.add_argument("--build-root", required=True, type=Path)
    args = parser.parse_args()

    repository = args.repository.resolve()
    manifest = json.loads(args.manifest.resolve().read_text(encoding="utf-8"))
    destination = args.destination.resolve()
    build_root = args.build_root.resolve()
    if not strict_child(destination, build_root):
        parser.error(f"destination must be a strict child of build root: {destination}")

    actual = git(repository, "rev-parse", "HEAD")
    expected = manifest["commit"]
    if actual != expected:
        parser.error(f"repository is at {actual}, expected {expected}")
    status = git(repository, "status", "--porcelain=v1", "--untracked-files=all")
    if status:
        parser.error("refusing to materialize a dirty source tree")

    if destination.exists():
        shutil.rmtree(destination)
    destination.mkdir(parents=True)
    build_root.mkdir(parents=True, exist_ok=True)

    with tempfile.NamedTemporaryFile(dir=build_root, suffix=".tar") as archive:
        subprocess.run(
            ["git", "-C", str(repository), "archive", "--format=tar", expected],
            check=True,
            stdout=archive,
        )
        archive.flush()
        archive.seek(0)
        with tarfile.open(fileobj=archive, mode="r:") as tar:
            for member in tar.getmembers():
                target = (destination / member.name).resolve()
                if not strict_child(target, destination):
                    parser.error(f"unsafe archive member: {member.name}")
            tar.extractall(destination, filter="data")

    metadata = {
        "schema_version": 1,
        "source_commit": expected,
        "source_repository": manifest["repository"],
        "generated_tree": True,
    }
    (destination / ".postgamma-source.json").write_text(
        json.dumps(metadata, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    print(f"materialized source tree {expected} -> {destination}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
