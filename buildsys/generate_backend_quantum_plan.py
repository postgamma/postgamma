#!/usr/bin/env python3
"""Generate the fail-closed PG19 ReadyForQuery quantum seam."""

from __future__ import annotations

import argparse
import hashlib
import json
from collections import defaultdict
from pathlib import Path
from typing import Any

from c_source_probe import CSourceProbeError, function_span, sanitize_c


MANIFEST_KIND = "postgamma.postgresql-backend-quantum"
EDIT_KINDS = {"replace", "insert_before", "insert_after", "insert_at"}


class BackendQuantumPlanError(ValueError):
    """The reviewed backend quantum seam no longer matches PostgreSQL."""


def load_manifest(path: Path) -> dict[str, Any]:
    document = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(document, dict) or set(document) != {
        "schema_version",
        "kind",
        "id",
        "product_postgresql_major",
        "edits",
    }:
        raise BackendQuantumPlanError("backend quantum manifest has an invalid schema")
    if (
        document["schema_version"] != 1
        or document["kind"] != MANIFEST_KIND
        or document["product_postgresql_major"] != 19
        or not isinstance(document["id"], str)
        or not document["id"]
        or not isinstance(document["edits"], list)
        or not document["edits"]
    ):
        raise BackendQuantumPlanError("backend quantum manifest identity is invalid")
    identifiers: set[str] = set()
    for index, edit in enumerate(document["edits"]):
        label = f"backend quantum edits[{index}]"
        common = {
            "id",
            "source_file",
            "enclosing_function",
            "anchor",
            "expected_matches",
            "kind",
            "replacement",
        }
        expected = common | ({"marker", "side"} if edit.get("kind") == "insert_at" else set())
        if not isinstance(edit, dict) or set(edit) != expected:
            raise BackendQuantumPlanError(f"{label} has an invalid schema")
        for field in ("id", "source_file", "anchor", "replacement"):
            if not isinstance(edit[field], str) or not edit[field]:
                raise BackendQuantumPlanError(f"{label}.{field} is invalid")
        if edit["id"] in identifiers:
            raise BackendQuantumPlanError(f"{label}.id is duplicated")
        identifiers.add(edit["id"])
        source_file = Path(edit["source_file"])
        if source_file.is_absolute() or ".." in source_file.parts:
            raise BackendQuantumPlanError(f"{label}.source_file is unsafe")
        if edit["enclosing_function"] is not None and (
            not isinstance(edit["enclosing_function"], str)
            or not edit["enclosing_function"]
        ):
            raise BackendQuantumPlanError(f"{label}.enclosing_function is invalid")
        if edit["kind"] not in EDIT_KINDS or edit["expected_matches"] != 1:
            raise BackendQuantumPlanError(f"{label} must define one supported edit")
        if edit["kind"] == "insert_at" and (
            not isinstance(edit["marker"], str)
            or not edit["marker"]
            or edit["side"] not in {"before", "after"}
            or edit["anchor"].count(edit["marker"]) != 1
        ):
            raise BackendQuantumPlanError(f"{label} has an invalid insertion marker")
    return document


def safe_source(source_root: Path, relative: str) -> Path:
    target = (source_root / relative).resolve()
    try:
        target.relative_to(source_root)
    except ValueError as exc:
        raise BackendQuantumPlanError(f"source path escapes PostgreSQL: {relative}") from exc
    if not target.is_file():
        raise BackendQuantumPlanError(f"backend quantum source is missing: {relative}")
    return target


def compile_plan(manifest: dict[str, Any], source_root: Path) -> dict[str, Any]:
    replacements_by_file: dict[str, list[dict[str, Any]]] = defaultdict(list)
    source_cache: dict[str, str] = {}
    for edit in manifest["edits"]:
        relative = edit["source_file"]
        raw = source_cache.setdefault(
            relative, safe_source(source_root, relative).read_text(encoding="utf-8")
        )
        scope_start = 0
        scope_end = len(raw)
        if edit["enclosing_function"] is not None:
            scope_start, scope_end = function_span(
                sanitize_c(raw), edit["enclosing_function"], 1
            )
        matches: list[int] = []
        offset = scope_start
        while True:
            offset = raw.find(edit["anchor"], offset, scope_end)
            if offset < 0:
                break
            matches.append(offset)
            offset += len(edit["anchor"])
        if len(matches) != edit["expected_matches"]:
            raise BackendQuantumPlanError(
                f"backend quantum edit {edit['id']} expected one match, found {len(matches)}"
            )
        match = matches[0]
        kind = edit["kind"]
        if kind == "replace":
            character_offset = match
            character_length = len(edit["anchor"])
            original = edit["anchor"]
        elif kind == "insert_before":
            character_offset = match
            character_length = 0
            original = ""
        elif kind == "insert_after":
            character_offset = match + len(edit["anchor"])
            character_length = 0
            original = ""
        else:
            marker_offset = edit["anchor"].index(edit["marker"])
            character_offset = match + marker_offset
            if edit["side"] == "after":
                character_offset += len(edit["marker"])
            character_length = 0
            original = ""
        byte_offset = len(raw[:character_offset].encode("utf-8"))
        byte_length = len(raw[
            character_offset : character_offset + character_length
        ].encode("utf-8"))
        replacements_by_file[relative].append(
            {
                "offset": byte_offset,
                "length": byte_length,
                "original": original,
                "replacement": edit["replacement"],
                "kind": "backend_quantum_seam",
                "rule_id": edit["id"],
                "use_kind": "semantic_hook",
                "line": raw.count("\n", 0, character_offset) + 1,
                "column": character_offset - raw.rfind("\n", 0, character_offset),
            }
        )

    files: list[dict[str, Any]] = []
    for relative in sorted(replacements_by_file):
        source = source_cache[relative].encode("utf-8")
        replacements = sorted(
            replacements_by_file[relative],
            key=lambda item: (item["offset"], item["length"], item["rule_id"]),
        )
        previous_end = -1
        previous_start = -1
        for replacement in replacements:
            start = replacement["offset"]
            end = start + replacement["length"]
            if start < previous_end or start == previous_start:
                raise BackendQuantumPlanError(
                    f"overlapping backend quantum edits in {relative} near {start}"
                )
            if source[start:end] != replacement["original"].encode("utf-8"):
                raise BackendQuantumPlanError(
                    f"backend quantum source spelling changed in {relative} at {start}"
                )
            previous_start = start
            previous_end = max(previous_end, end)
        files.append(
            {
                "path": relative,
                "sha256": hashlib.sha256(source).hexdigest(),
                "replacements": replacements,
            }
        )
    return {
        "schema_version": 1,
        "mode": "plan",
        "kind": "postgamma.backend-quantum-plan",
        "source_root": ".",
        "manifest_id": manifest["id"],
        "files": files,
        "summary": {
            "file_count": len(files),
            "replacement_count": sum(
                len(entry["replacements"]) for entry in files
            ),
            "ready_for_query_yield_points": 1,
            "command_read_yield_points": 1,
        },
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--manifest", required=True, type=Path)
    parser.add_argument("--source-root", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    try:
        plan = compile_plan(
            load_manifest(args.manifest.resolve()), args.source_root.resolve()
        )
    except (OSError, json.JSONDecodeError, CSourceProbeError, BackendQuantumPlanError) as exc:
        parser.error(str(exc))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(
        json.dumps(plan, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    print(
        "backend quantum plan: "
        f"{plan['summary']['replacement_count']} edits in "
        f"{plan['summary']['file_count']} files"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
