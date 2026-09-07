#!/usr/bin/env python3
"""Reconcile source shared by backend and frontend-tool state domains."""

from __future__ import annotations

import argparse
import json
import re
from pathlib import Path
from typing import Any


BACKEND_PATTERN = re.compile(
    r"^POSTGAMMA_BACKEND_STATE_VALUE\(POSTGAMMA_BACKEND_STATE_SLOT_"
    r"([0-9A-F]{16}), ([A-Za-z_][A-Za-z0-9_]*)\)$"
)
TOOL_PATTERN = re.compile(
    r"^POSTGAMMA_TOOL_STATE_VALUE\(POSTGAMMA_TOOL_STATE_SLOT_"
    r"([0-9A-F]{16}), ([A-Za-z_][A-Za-z0-9_]*)\)$"
)


class ReconcileError(ValueError):
    """State plans disagree outside the supported domain intersection."""


def load_plan(path: Path, expected_kind: str) -> dict[str, Any]:
    try:
        plan = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise ReconcileError(f"cannot read {path}: {exc}") from exc
    if (
        not isinstance(plan, dict)
        or plan.get("schema_version") != 1
        or plan.get("mode") != "plan"
        or plan.get("kind") != expected_kind
        or not isinstance(plan.get("files"), list)
    ):
        raise ReconcileError(f"{path}: invalid {expected_kind} document")
    return plan


def replacement_map(
    plan: dict[str, Any], label: str
) -> tuple[dict[tuple[str, int, int], dict[str, Any]], dict[str, str]]:
    result: dict[tuple[str, int, int], dict[str, Any]] = {}
    digests: dict[str, str] = {}
    for file_entry in plan["files"]:
        if not isinstance(file_entry, dict):
            raise ReconcileError(f"{label}: invalid file entry")
        path = file_entry.get("path")
        digest = file_entry.get("sha256")
        replacements = file_entry.get("replacements")
        if (
            not isinstance(path, str)
            or not isinstance(digest, str)
            or not isinstance(replacements, list)
        ):
            raise ReconcileError(f"{label}: invalid file facts")
        if path in digests and digests[path] != digest:
            raise ReconcileError(f"{label}: inconsistent digest for {path}")
        digests[path] = digest
        for replacement in replacements:
            if not isinstance(replacement, dict):
                raise ReconcileError(f"{label}: invalid replacement in {path}")
            key = (path, replacement.get("offset"), replacement.get("length"))
            if not isinstance(key[1], int) or not isinstance(key[2], int):
                raise ReconcileError(f"{label}: invalid replacement location")
            if key in result:
                raise ReconcileError(f"{label}: duplicate replacement at {key}")
            result[key] = replacement
    return result, digests


def shared_replacement(
    backend: dict[str, Any], tool: dict[str, Any], key: tuple[str, int, int]
) -> dict[str, Any]:
    if (
        backend.get("original") != tool.get("original")
        or backend.get("rule_id") != tool.get("rule_id")
    ):
        raise ReconcileError(
            f"state-domain collision has different symbols at {key}"
        )
    backend_match = BACKEND_PATTERN.fullmatch(backend.get("replacement", ""))
    tool_match = TOOL_PATTERN.fullmatch(tool.get("replacement", ""))
    if backend_match is None or tool_match is None:
        raise ReconcileError(
            f"state-domain collision needs an unsupported relocation at {key}"
        )
    if backend_match.groups() != tool_match.groups():
        raise ReconcileError(f"state-domain slot identity differs at {key}")
    digest, name = backend_match.groups()
    merged = dict(backend)
    merged.update(
        {
            "replacement": (
                f"POSTGAMMA_CONTEXT_STATE_VALUE({digest}, {name})"
            ),
            "kind": "context_state",
            "use_kind": (
                backend.get("use_kind")
                if backend.get("use_kind") == tool.get("use_kind")
                else "mixed"
            ),
            "domains": ["backend", "frontend-tool"],
        }
    )
    return merged


def reconcile(
    backend_plan: dict[str, Any], tool_plan: dict[str, Any]
) -> dict[str, Any]:
    backend, backend_digests = replacement_map(backend_plan, "backend")
    tool, tool_digests = replacement_map(tool_plan, "frontend-tool")
    for path in set(backend_digests) & set(tool_digests):
        if backend_digests[path] != tool_digests[path]:
            raise ReconcileError(f"source digest differs between domains for {path}")
    all_keys = sorted(set(backend) | set(tool))
    reconciled: dict[str, list[dict[str, Any]]] = {}
    intersections = 0
    for key in all_keys:
        if key in backend and key in tool:
            replacement = shared_replacement(backend[key], tool[key], key)
            intersections += 1
        else:
            replacement = backend.get(key, tool.get(key))
            assert replacement is not None
        reconciled.setdefault(key[0], []).append(replacement)
    files = []
    for path in sorted(reconciled):
        files.append(
            {
                "path": path,
                "sha256": backend_digests.get(path, tool_digests.get(path)),
                "replacements": sorted(
                    reconciled[path],
                    key=lambda value: (
                        value["offset"],
                        value["length"],
                        value["rule_id"],
                    ),
                ),
            }
        )
    return {
        "schema_version": 1,
        "mode": "plan",
        "kind": "postgamma.context-state-plan",
        "source_root": ".",
        "files": files,
        "summary": {
            "file_count": len(files),
            "replacement_count": len(all_keys),
            "shared_domain_replacement_count": intersections,
        },
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--backend-plan", required=True, type=Path)
    parser.add_argument("--tool-plan", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    try:
        result = reconcile(
            load_plan(args.backend_plan, "postgamma.backend-state-plan"),
            load_plan(args.tool_plan, "postgamma.tool-state-plan"),
        )
    except ReconcileError as exc:
        parser.error(str(exc))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(
        json.dumps(result, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    print(
        "context state plan: "
        f"{result['summary']['replacement_count']} replacement(s), "
        f"{result['summary']['shared_domain_replacement_count']} shared"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
