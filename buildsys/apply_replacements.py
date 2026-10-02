#!/usr/bin/env python3
"""Apply a fail-closed AST replacement plan to a materialized tree."""

from __future__ import annotations

import argparse
import hashlib
import json
import tempfile
from pathlib import Path, PurePosixPath


def safe_target(tree: Path, relative: str) -> Path:
    logical = PurePosixPath(relative)
    if logical.is_absolute() or ".." in logical.parts:
        raise ValueError(f"unsafe plan path: {relative}")
    target = (tree / Path(*logical.parts)).resolve()
    try:
        target.relative_to(tree)
    except ValueError as exc:
        raise ValueError(f"plan path escapes generated tree: {relative}") from exc
    return target


def apply_bytes(source: bytes, entry: dict[str, object]) -> tuple[bytes, int]:
    actual_hash = hashlib.sha256(source).hexdigest()
    expected_hash = str(entry["sha256"])
    if actual_hash != expected_hash:
        raise ValueError(
            f"source hash mismatch for {entry['path']}: expected {expected_hash}, got {actual_hash}"
        )

    replacements = sorted(
        entry["replacements"], key=lambda item: (int(item["offset"]), int(item["length"])),
        reverse=True,
    )
    previous_start = len(source) + 1
    result = source
    for replacement in replacements:
        start = int(replacement["offset"])
        length = int(replacement["length"])
        end = start + length
        if start < 0 or end > len(source):
            raise ValueError(f"replacement outside file bounds: {entry['path']}:{start}+{length}")
        if end > previous_start:
            raise ValueError(f"overlapping replacements in {entry['path']} near byte {start}")
        original = str(replacement["original"]).encode("utf-8")
        if source[start:end] != original:
            raise ValueError(
                f"replacement spelling mismatch in {entry['path']} at byte {start}: "
                f"expected {original!r}, got {source[start:end]!r}"
            )
        result = result[:start] + str(replacement["replacement"]).encode("utf-8") + result[end:]
        previous_start = start

    return result, len(replacements)


def apply_file(target: Path, entry: dict[str, object]) -> int:
    result, replacement_count = apply_bytes(target.read_bytes(), entry)
    with tempfile.NamedTemporaryFile(dir=target.parent, prefix=".postgamma-", delete=False) as handle:
        handle.write(result)
        temporary = Path(handle.name)
    temporary.chmod(target.stat().st_mode)
    temporary.replace(target)
    return replacement_count


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--plan", required=True, type=Path)
    parser.add_argument("--tree", required=True, type=Path)
    args = parser.parse_args()
    tree = args.tree.resolve()
    if not (tree / ".postgamma-source.json").is_file():
        parser.error(f"not a PostGamma materialized tree: {tree}")
    plan_bytes = args.plan.read_bytes()
    plan = json.loads(plan_bytes)
    if plan.get("schema_version") != 1 or plan.get("mode") != "plan":
        parser.error("unsupported replacement plan")

    total = 0
    try:
        for entry in sorted(plan["files"], key=lambda item: item["path"]):
            target = safe_target(tree, str(entry["path"]))
            if not target.is_file():
                raise ValueError(f"planned source does not exist: {entry['path']}")
            total += apply_file(target, entry)
    except (KeyError, TypeError, ValueError) as exc:
        parser.error(str(exc))

    provenance = {
        "schema_version": 1,
        "plan_sha256": hashlib.sha256(plan_bytes).hexdigest(),
        "replacement_count": total,
    }
    (tree / ".postgamma-replacements.json").write_text(
        json.dumps(provenance, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    print(f"applied {total} semantic replacement(s) to {tree}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
