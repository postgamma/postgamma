#!/usr/bin/env python3
"""Merge independently generated semantic plans without weakening checks."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
from typing import Any


def load_plan(path: Path) -> tuple[bytes, dict[str, Any]]:
    data = path.read_bytes()
    document = json.loads(data)
    if (
        not isinstance(document, dict)
        or document.get("schema_version") != 1
        or document.get("mode") != "plan"
        or not isinstance(document.get("files"), list)
    ):
        raise ValueError(f"{path}: unsupported replacement plan")
    return data, document


def merge(paths: list[Path]) -> dict[str, Any]:
    if len(paths) < 2:
        raise ValueError("at least two input plans are required")
    by_path: dict[str, dict[str, Any]] = {}
    components = []
    for path in paths:
        data, plan = load_plan(path)
        components.append(
            {
                "path": path.name,
                "kind": plan.get("kind", "postgamma.ast-plan"),
                "sha256": hashlib.sha256(data).hexdigest(),
                "file_count": len(plan["files"]),
            }
        )
        for entry in plan["files"]:
            if not isinstance(entry, dict):
                raise ValueError(f"{path}: file entry must be an object")
            logical = entry.get("path")
            digest = entry.get("sha256")
            replacements = entry.get("replacements")
            if (
                not isinstance(logical, str)
                or not isinstance(digest, str)
                or not isinstance(replacements, list)
            ):
                raise ValueError(f"{path}: invalid file entry")
            target = by_path.setdefault(
                logical, {"path": logical, "sha256": digest, "replacements": []}
            )
            if target["sha256"] != digest:
                raise ValueError(f"source digest disagreement for {logical}")
            target["replacements"].extend(replacements)

    total = 0
    files = []
    for logical in sorted(by_path):
        entry = by_path[logical]
        unique: dict[tuple[int, int], dict[str, Any]] = {}
        for replacement in entry["replacements"]:
            if not isinstance(replacement, dict):
                raise ValueError(f"{logical}: replacement must be an object")
            try:
                key = (int(replacement["offset"]), int(replacement["length"]))
            except (KeyError, TypeError, ValueError) as exc:
                raise ValueError(f"{logical}: invalid replacement location") from exc
            previous = unique.get(key)
            if previous is not None and previous != replacement:
                raise ValueError(
                    f"conflicting replacements in {logical} at {key[0]}+{key[1]}"
                )
            unique[key] = replacement
        replacements = sorted(
            unique.values(),
            key=lambda item: (int(item["offset"]), int(item["length"]), str(item.get("rule_id", ""))),
        )
        previous_end = -1
        previous_start = -1
        for replacement in replacements:
            start = int(replacement["offset"])
            end = start + int(replacement["length"])
            # Even a zero-length insertion at the start of another edit is
            # order-dependent when plans are composed.  Reject every shared
            # start offset; exact duplicates were already collapsed above.
            if start < previous_end or start == previous_start:
                raise ValueError(f"overlapping replacements in {logical} near byte {start}")
            previous_start = start
            previous_end = max(previous_end, end)
        total += len(replacements)
        files.append(
            {
                "path": logical,
                "sha256": entry["sha256"],
                "replacements": replacements,
            }
        )
    return {
        "schema_version": 1,
        "mode": "plan",
        "kind": "postgamma.combined-plan",
        "source_root": ".",
        "components": components,
        "files": files,
        "summary": {
            "component_count": len(components),
            "file_count": len(files),
            "replacement_count": total,
        },
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--plan", action="append", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    try:
        result = merge(args.plan)
    except (OSError, json.JSONDecodeError, ValueError) as exc:
        parser.error(str(exc))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(
        json.dumps(result, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    print(
        f"combined plan: {result['summary']['replacement_count']} replacements "
        f"in {result['summary']['file_count']} files"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
