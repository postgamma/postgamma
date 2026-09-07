#!/usr/bin/env python3
"""Prove the resource pack is complete, safe, and reproducible."""

from __future__ import annotations

import argparse
import hashlib
import json
import tempfile
from pathlib import Path
from typing import Any

from build_resource_pack import (
    PACK_KIND,
    REQUIRED_LAYOUT_DIRECTORIES,
    ResourcePackError,
    build_resource_pack,
    validate_logical_path,
)
from bundle_embedded_static_modules import read_json, validate_manifest
from check_embedded_lifecycle import input_identity, write_json


EVIDENCE_KIND = "postgamma.resource-pack"


class ResourcePackCheckError(RuntimeError):
    """The generated resource pack is incomplete or nondeterministic."""


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def actual_files(root: Path) -> dict[str, Path]:
    return {
        path.relative_to(root).as_posix(): path
        for path in sorted(root.rglob("*"))
        if path.is_file()
    }


def actual_directories(root: Path) -> set[str]:
    return {
        path.relative_to(root).as_posix()
        for path in sorted(root.rglob("*"))
        if path.is_dir()
    }


def validate_pack(root: Path, receipt: dict[str, Any]) -> dict[str, Any]:
    if receipt.get("schema_version") != 1 or receipt.get("kind") != PACK_KIND:
        raise ResourcePackCheckError("resource-pack receipt identity is invalid")
    if receipt.get("required_layout_directories") != list(
        REQUIRED_LAYOUT_DIRECTORIES
    ):
        raise ResourcePackCheckError("resource-pack layout contract is invalid")
    raw_files = receipt.get("files")
    if not isinstance(raw_files, list) or receipt.get("file_count") != len(raw_files):
        raise ResourcePackCheckError("resource-pack file inventory is invalid")
    expected: dict[str, dict[str, Any]] = {}
    absolute_logical_paths = 0
    for entry in raw_files:
        if not isinstance(entry, dict) or not isinstance(
            entry.get("logical_path"), str
        ):
            raise ResourcePackCheckError("resource-pack file entry is invalid")
        logical = entry["logical_path"]
        if logical.startswith("/"):
            absolute_logical_paths += 1
        validate_logical_path(logical)
        if logical in expected:
            raise ResourcePackCheckError("resource-pack logical path is unsafe")
        expected[logical] = entry
    observed = actual_files(root)
    observed_directories = actual_directories(root)
    missing_directories = set(REQUIRED_LAYOUT_DIRECTORIES) - observed_directories
    if missing_directories:
        raise ResourcePackCheckError(
            "resource pack is missing required layout directories: "
            + ", ".join(sorted(missing_directories))
        )
    metadata_name = "share/postgamma/resource-pack.json"
    if set(observed) != set(expected) | {metadata_name}:
        raise ResourcePackCheckError("resource pack has missing or extra files")
    for logical, entry in expected.items():
        path = observed[logical]
        if (
            path.stat().st_size != entry.get("size")
            or (path.stat().st_mode & 0o777) != entry.get("mode")
            or sha256(path) != entry.get("sha256")
        ):
            raise ResourcePackCheckError(f"resource identity changed: {logical}")
    embedded = json.loads(observed[metadata_name].read_text(encoding="utf-8"))
    if embedded != receipt:
        raise ResourcePackCheckError("embedded and external pack receipts differ")
    return {
        "file_count": len(expected),
        "tree_sha256": receipt.get("tree_sha256"),
        "absolute_logical_paths": absolute_logical_paths,
        "missing_files": 0,
        "extra_files": 0,
        "metadata_matches_receipt": True,
        "required_layout_directories": list(REQUIRED_LAYOUT_DIRECTORIES),
    }


def compare_trees(first: Path, second: Path) -> None:
    first_files = actual_files(first)
    second_files = actual_files(second)
    if set(first_files) != set(second_files):
        raise ResourcePackCheckError("repeat build changed resource paths")
    if actual_directories(first) != actual_directories(second):
        raise ResourcePackCheckError("repeat build changed resource directories")
    for logical in first_files:
        first_path = first_files[logical]
        second_path = second_files[logical]
        if (
            first_path.read_bytes() != second_path.read_bytes()
            or (first_path.stat().st_mode & 0o777)
            != (second_path.stat().st_mode & 0o777)
        ):
            raise ResourcePackCheckError(
                f"repeat build changed resource bytes: {logical}"
            )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--project-root", required=True, type=Path)
    parser.add_argument("--install-root", required=True, type=Path)
    parser.add_argument("--manifest", required=True, type=Path)
    parser.add_argument("--upstream", required=True, type=Path)
    parser.add_argument("--pack-root", required=True, type=Path)
    parser.add_argument("--receipt", required=True, type=Path)
    parser.add_argument("--work-root", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    try:
        project_root = args.project_root.resolve(strict=True)
        install_root = args.install_root.resolve(strict=True)
        manifest_path = args.manifest.resolve(strict=True)
        upstream_path = args.upstream.resolve(strict=True)
        pack_root = args.pack_root.resolve(strict=True)
        receipt_path = args.receipt.resolve(strict=True)
        receipt = read_json(receipt_path)
        pack = validate_pack(pack_root, receipt)
        manifest = validate_manifest(read_json(manifest_path))
        declared = sum(len(module["resources"]) for module in manifest["modules"])
        work_root = args.work_root.resolve()
        work_root.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="release-pack-", dir=work_root) as temp:
            repeated_root = Path(temp) / "pack"
            repeated_receipt = Path(temp) / "receipt.json"
            repeated = build_resource_pack(
                project_root,
                install_root,
                manifest_path,
                upstream_path,
                repeated_root,
                repeated_receipt,
                Path(temp),
            )
            if repeated != receipt:
                raise ResourcePackCheckError("repeat build changed pack receipt")
            compare_trees(pack_root, repeated_root)
        document = {
            "schema_version": 1,
            "kind": EVIDENCE_KIND,
            "status": "pass",
            "postgresql_major": 19,
            "pack": pack,
            "declared_extension_resources": declared,
            "deterministic_rebuilds": 2,
            "inputs": input_identity(
                [manifest_path, upstream_path, receipt_path]
            ),
        }
        write_json(args.output.resolve(), document)
    except (OSError, ResourcePackCheckError, ResourcePackError, ValueError) as exc:
        parser.error(str(exc))
    print(
        "resource pack evidence: pass "
        "(complete installed layout, safe paths, deterministic rebuild)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
