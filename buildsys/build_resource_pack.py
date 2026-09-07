#!/usr/bin/env python3
"""Build the deterministic installed-layout resource pack for embedded release."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import tempfile
from pathlib import Path
from typing import Any

from bundle_embedded_static_modules import (
    StaticModuleBundleError,
    read_json,
    safe_relative_path,
    validate_manifest,
)
from reset_directory import is_strict_child


PACK_KIND = "postgamma.embedded-resource-pack"
REQUIRED_LAYOUT_DIRECTORIES = ("bin", "lib", "share")


class ResourcePackError(RuntimeError):
    """An installed tree or declared resource cannot form a safe pack."""


def sha256_bytes(content: bytes) -> str:
    return hashlib.sha256(content).hexdigest()


def validate_logical_path(value: str) -> str:
    try:
        return safe_relative_path(value, "resource logical path")
    except StaticModuleBundleError as exc:
        raise ResourcePackError(f"unsafe resource path: {value!r}") from exc


def find_installed_prefix(install_root: Path) -> Path:
    candidates = sorted(install_root.glob("**/bin/postgres"))
    if len(candidates) != 1:
        raise ResourcePackError(
            "expected exactly one installed bin/postgres, found "
            f"{len(candidates)}"
        )
    return candidates[0].parent.parent


def normalized_mode(source: Path) -> int:
    return 0o755 if source.stat().st_mode & 0o111 else 0o644


def install_file(source: Path, target: Path, expected: bytes | None = None) -> bytes:
    if not source.is_file():
        raise ResourcePackError(f"resource source is missing: {source}")
    content = source.read_bytes()
    if expected is not None and content != expected:
        raise ResourcePackError(f"resource overlay conflicts with {target}")
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(content)
    target.chmod(normalized_mode(source))
    os.utime(target, (0, 0), follow_symlinks=False)
    return content


def tree_digest(entries: list[dict[str, Any]]) -> str:
    digest = hashlib.sha256()
    for entry in entries:
        digest.update(entry["logical_path"].encode("utf-8"))
        digest.update(b"\0")
        digest.update(str(entry["mode"]).encode("ascii"))
        digest.update(b"\0")
        digest.update(str(entry["size"]).encode("ascii"))
        digest.update(b"\0")
        digest.update(entry["sha256"].encode("ascii"))
        digest.update(b"\n")
    return digest.hexdigest()


def build_resource_pack(
    project_root: Path,
    install_root: Path,
    manifest_path: Path,
    upstream_path: Path,
    output_root: Path,
    receipt_path: Path,
    within_root: Path,
) -> dict[str, Any]:
    project_root = project_root.resolve(strict=True)
    install_root = install_root.resolve(strict=True)
    manifest = validate_manifest(read_json(manifest_path))
    if manifest["schema_version"] != 2:
        raise ResourcePackError("the product resource pack requires manifest v2")
    upstream = read_json(upstream_path)
    if (
        upstream.get("schema_version") != 1
        or not isinstance(upstream.get("commit"), str)
        or len(upstream["commit"]) != 40
    ):
        raise ResourcePackError("upstream identity is invalid")
    if manifest["postgresql_major"] != 19:
        raise ResourcePackError("resource pack PostgreSQL major is not 19")
    prefix = find_installed_prefix(install_root)
    if output_root.is_symlink() or receipt_path.is_symlink():
        raise ResourcePackError("resource-pack outputs may not be symbolic links")
    output_root = output_root.resolve()
    receipt_path = receipt_path.resolve()
    within_root = within_root.resolve(strict=True)
    if not is_strict_child(output_root, within_root) or not is_strict_child(
        receipt_path, within_root
    ):
        raise ResourcePackError(
            "resource-pack outputs are not strict children of their build root"
        )
    output_root.parent.mkdir(parents=True, exist_ok=True)
    consumers: dict[str, set[str]] = {}
    required: dict[str, bool] = {}
    with tempfile.TemporaryDirectory(
        prefix=f".{output_root.name}.", dir=output_root.parent
    ) as temporary:
        staging = Path(temporary) / "pack"
        staging.mkdir()
        for logical in REQUIRED_LAYOUT_DIRECTORIES:
            directory = staging / logical
            directory.mkdir()
            os.utime(directory, (0, 0), follow_symlinks=False)
        installed_sources = [prefix / "bin" / "postgres"]
        share = prefix / "share"
        if not share.is_dir():
            raise ResourcePackError("installed PostgreSQL share directory is missing")
        installed_sources.extend(
            path for path in sorted(share.rglob("*")) if path.is_file()
        )
        for source in installed_sources:
            relative = source.relative_to(prefix).as_posix()
            validate_logical_path(relative)
            install_file(source, staging / relative)
            consumers.setdefault(relative, set()).add("postgresql-runtime")
            required[relative] = True
        for module in manifest["modules"]:
            for resource in module["resources"]:
                logical = resource["logical_path"]
                validate_logical_path(logical)
                source = (project_root / resource["source"]).resolve()
                try:
                    source.relative_to(project_root)
                except ValueError as exc:
                    raise ResourcePackError(
                        f"resource source escapes project root: {source}"
                    ) from exc
                target = staging / logical
                existing = target.read_bytes() if target.is_file() else None
                install_file(source, target, existing)
                consumers.setdefault(logical, set()).add(module["id"])
                required[logical] = required.get(logical, False) or resource["required"]
        entries: list[dict[str, Any]] = []
        for path in sorted(staging.rglob("*")):
            if not path.is_file():
                continue
            logical = path.relative_to(staging).as_posix()
            content = path.read_bytes()
            entries.append(
                {
                    "logical_path": logical,
                    "size": len(content),
                    "sha256": sha256_bytes(content),
                    "mode": path.stat().st_mode & 0o777,
                    "required": required.get(logical, True),
                    "consumers": sorted(consumers.get(logical, {"postgresql-runtime"})),
                }
            )
        document: dict[str, Any] = {
            "schema_version": 1,
            "kind": PACK_KIND,
            "postgresql_major": manifest["postgresql_major"],
            "upstream_commit": upstream["commit"],
            "module_manifest_id": manifest["id"],
            "module_manifest_sha256": sha256_bytes(manifest_path.read_bytes()),
            "required_layout_directories": list(REQUIRED_LAYOUT_DIRECTORIES),
            "file_count": len(entries),
            "tree_sha256": tree_digest(entries),
            "files": entries,
        }
        metadata = staging / "share" / "postgamma" / "resource-pack.json"
        metadata.parent.mkdir(parents=True, exist_ok=True)
        metadata.write_text(
            json.dumps(document, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
        metadata.chmod(0o644)
        os.utime(metadata, (0, 0), follow_symlinks=False)
        if output_root.exists():
            shutil.rmtree(output_root)
        os.replace(staging, output_root)
    receipt_path.parent.mkdir(parents=True, exist_ok=True)
    receipt_path.write_text(
        json.dumps(document, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    return document


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--project-root", required=True, type=Path)
    parser.add_argument("--install-root", required=True, type=Path)
    parser.add_argument("--manifest", required=True, type=Path)
    parser.add_argument("--upstream", required=True, type=Path)
    parser.add_argument("--output-root", required=True, type=Path)
    parser.add_argument("--receipt", required=True, type=Path)
    parser.add_argument("--within-root", required=True, type=Path)
    args = parser.parse_args()
    try:
        document = build_resource_pack(
            args.project_root,
            args.install_root,
            args.manifest,
            args.upstream,
            args.output_root,
            args.receipt,
            args.within_root,
        )
    except (OSError, ResourcePackError) as exc:
        parser.error(str(exc))
    print(
        "embedded resource pack: "
        f"{document['file_count']} file(s), {document['tree_sha256']}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
