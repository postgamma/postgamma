#!/usr/bin/env python3
"""Fail closed unless the PostgreSQL input is pinned and completely clean."""

from __future__ import annotations

import argparse
import configparser
import json
import subprocess
from pathlib import Path

from postgresql_adapter import (
    AdapterError,
    inspect_source_compatibility,
    load_adapter,
    validate_source_compatibility,
)


def git(repository: Path, *arguments: str) -> str:
    result = subprocess.run(
        ["git", "-C", str(repository), *arguments],
        check=False,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    if result.returncode:
        raise RuntimeError(result.stderr.strip() or result.stdout.strip())
    return result.stdout.strip()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", required=True, type=Path)
    parser.add_argument("--manifest", required=True, type=Path)
    parser.add_argument("--repository", type=Path)
    parser.add_argument("--adapter", required=True, type=Path)
    parser.add_argument("--candidate-probe", action="store_true")
    args = parser.parse_args()
    root = args.root.resolve()
    manifest_path = args.manifest if args.manifest.is_absolute() else root / args.manifest
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    repository = (
        args.repository.resolve()
        if args.repository is not None
        else (root / manifest["path"]).resolve()
    )

    if not (repository / ".git").exists():
        parser.error(f"submodule is not initialized: {repository}")

    try:
        actual_commit = git(repository, "rev-parse", "HEAD")
        status = git(repository, "status", "--porcelain=v1", "--untracked-files=all")
    except RuntimeError as exc:
        parser.error(str(exc))

    expected_commit = manifest["commit"]
    if actual_commit != expected_commit:
        parser.error(f"PostgreSQL commit mismatch: expected {expected_commit}, got {actual_commit}")
    if status:
        parser.error("PostgreSQL submodule is dirty:\n" + status)

    try:
        adapter = load_adapter(args.adapter.resolve())
        if args.candidate_probe:
            major, _declared = inspect_source_compatibility(adapter, repository)
        else:
            major = validate_source_compatibility(adapter, repository)
    except AdapterError as exc:
        parser.error(str(exc))

    modules = configparser.ConfigParser()
    modules.read(root / ".gitmodules", encoding="utf-8")
    section = 'submodule "postgres"'
    if section not in modules:
        parser.error(".gitmodules has no postgres submodule")
    configured_url = modules[section].get("url")
    if configured_url != manifest["repository"]:
        parser.error(
            f"submodule URL mismatch: expected {manifest['repository']}, got {configured_url}"
        )

    if args.candidate_probe:
        declared = ", ".join(
            str(value) for value in adapter["supported_postgresql_majors"]
        )
        print(
            f"upstream candidate probe: ok ({expected_commit}, PostgreSQL {major}; "
            f"product majors: {declared})"
        )
    else:
        print(f"upstream: ok ({expected_commit})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
