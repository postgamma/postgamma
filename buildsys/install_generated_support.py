#!/usr/bin/env python3
"""Install private generated runtime files into a disposable PostgreSQL tree."""

from __future__ import annotations

import argparse
import hashlib
import json
import shutil
from pathlib import Path

from postgresql_adapter import (
    AdapterError,
    SUPPORT_PROFILES,
    apply_non_overlapping_edits,
    adapter_sha256,
    format_anchor_failures,
    generated_support_edits,
    inspect_generated_support_anchors,
    load_adapter,
    inspect_source_compatibility,
    validate_source_compatibility,
)


class InstallError(ValueError):
    """Generated support cannot be installed without an ambiguous source edit."""


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def install_support(
    root: Path,
    runtime_dir: Path,
    tree: Path,
    adapter_path: Path,
    profile: str = "validation",
    candidate_probe: bool = False,
) -> dict[str, object]:
    root = root.resolve()
    runtime_dir = runtime_dir.resolve()
    tree = tree.resolve()
    adapter_path = adapter_path.resolve()
    if not (tree / ".postgamma-replacements.json").is_file():
        raise InstallError("replacement provenance missing; apply the AST plan first")

    adapter = load_adapter(adapter_path)
    if candidate_probe:
        source_major, _declared = inspect_source_compatibility(adapter, tree)
    else:
        source_major = validate_source_compatibility(adapter, tree)
    source_roots = {"project": root, "runtime": runtime_dir}
    copy_requests: list[tuple[Path, Path]] = []
    for copy in adapter["generated_support"]["copies"]:
        source = source_roots[copy["source_root"]] / copy["source"]
        target = tree / copy["target"]
        copy_requests.append((source, target))
    missing = [source for source, _ in copy_requests if not source.is_file()]
    if missing:
        raise InstallError(
            "generated runtime input(s) missing: "
            + ", ".join(str(path) for path in missing)
        )

    # Inspect every immutable upstream anchor before making any filesystem
    # change. This keeps an incompatible adapter from leaving a partially
    # installed generated tree behind.
    anchor_records = inspect_generated_support_anchors(adapter, tree, profile)
    failures = [record for record in anchor_records if record["status"] != "ok"]
    if failures:
        raise InstallError(format_anchor_failures(failures))

    previous_installed: set[str] = set()
    previous_metadata_path = tree / ".postgamma-support.json"
    if previous_metadata_path.is_file():
        try:
            previous_metadata = json.loads(
                previous_metadata_path.read_text(encoding="utf-8")
            )
        except (OSError, json.JSONDecodeError) as exc:
            raise InstallError(
                f"cannot load previous generated support metadata: {exc}"
            ) from exc
        installed_entries = previous_metadata.get("installed")
        if not isinstance(installed_entries, list) or any(
            not isinstance(entry, str) for entry in installed_entries
        ):
            raise InstallError("previous generated support metadata is invalid")
        previous_installed = set(installed_entries)
    collisions = [
        target
        for _source, target in copy_requests
        if target.exists()
        and target.relative_to(tree).as_posix() not in previous_installed
    ]
    if collisions:
        raise InstallError(
            "generated runtime target(s) already exist: "
            + ", ".join(str(path) for path in collisions)
        )

    installed: list[str] = []
    for source, target in copy_requests:
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, target)
        installed.append(target.relative_to(tree).as_posix())

    edits_by_path: dict[str, list[dict[str, object]]] = {}
    for _edit_class, edit in generated_support_edits(adapter, profile):
        edits_by_path.setdefault(edit["path"], []).append(edit)
    edited: list[str] = []
    for relative, edits in sorted(edits_by_path.items()):
        target = tree / relative
        try:
            text = target.read_text(encoding="utf-8")
        except OSError as exc:
            raise InstallError(f"cannot read {target}: {exc}") from exc
        try:
            transformed = apply_non_overlapping_edits(edits, text)
        except AdapterError as exc:
            raise InstallError(f"{relative}: {exc}") from exc
        target.write_text(transformed, encoding="utf-8")
        edited.append(relative)

    installed.sort()
    edited = sorted(set(edited))
    metadata = {
        "schema_version": 1,
        "adapter": {
            "id": adapter["id"],
            "sha256": adapter_sha256(adapter_path),
            "postgresql_major": source_major,
            "support_profile": profile,
        },
        "installed": installed,
        "edited": edited,
        "edit_regions": [
            {
                key: record[key]
                for key in (
                    "id",
                    "class",
                    "path",
                    "mode",
                    "matched_region_length",
                    "matched_region_sha256",
                    "spans",
                )
                if key in record
            }
            for record in anchor_records
        ],
        "sha256": {
            relative: sha256(tree / relative) for relative in sorted(installed + edited)
        },
    }
    (tree / ".postgamma-support.json").write_text(
        json.dumps(metadata, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    print(f"installed generated runtime support into {tree}")
    return metadata


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--project-root", required=True, type=Path)
    parser.add_argument("--runtime-dir", required=True, type=Path)
    parser.add_argument("--tree", required=True, type=Path)
    parser.add_argument("--adapter", required=True, type=Path)
    parser.add_argument("--profile", choices=sorted(SUPPORT_PROFILES), default="validation")
    parser.add_argument("--candidate-probe", action="store_true")
    args = parser.parse_args()
    try:
        install_support(
            args.project_root,
            args.runtime_dir,
            args.tree,
            args.adapter,
            args.profile,
            args.candidate_probe,
        )
    except (AdapterError, InstallError) as exc:
        parser.error(str(exc))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
