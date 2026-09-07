#!/usr/bin/env python3
"""Generate PostgreSQL-dependent frontend-tool state layout facts."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

from check_frontend_tool_state_policy import (
    FrontendToolStatePolicyError,
    policy_decisions,
    validate_alignment,
)
from state_policy import catalog_candidates, load_json


RUNTIME_KIND = "postgamma.tool-state-runtime"


@dataclass
class Slot:
    identifier: str
    name: str
    canonical_type: str
    enum_name: str
    offset: int
    size: int
    alignment: int
    copy_template: bool
    relocations: list[str] = field(default_factory=list)
    relocation_counts: dict[str, int] = field(default_factory=dict)


def align_up(value: int, alignment: int) -> int:
    return (value + alignment - 1) // alignment * alignment


def slot_enum(identifier: str) -> str:
    digest = hashlib.sha256(identifier.encode("utf-8")).hexdigest()[:16].upper()
    return f"POSTGAMMA_TOOL_STATE_SLOT_{digest}"


def relocation_bridge_name(identifier: str) -> str:
    digest = hashlib.sha256(identifier.encode("utf-8")).hexdigest()[:16]
    return f"postgamma_tool_state_relocated_{digest}"


def compile_slots(
    catalog: dict[str, Any],
    policy: dict[str, Any],
    alignment: dict[str, Any],
) -> tuple[list[Slot], int, int]:
    candidates = catalog_candidates(catalog, FrontendToolStatePolicyError)
    decisions = policy_decisions(policy)
    tool_ids = sorted(
        identifier
        for identifier in candidates
        if decisions[identifier]["owner"] == "tool"
    )
    slots: list[Slot] = []
    offset = 0
    maximum_alignment = 1
    for identifier in tool_ids:
        candidate = candidates[identifier]
        item_alignment = int(candidate["alignment"])
        item_size = int(candidate["size"])
        if item_alignment <= 0 or item_size <= 0:
            raise FrontendToolStatePolicyError(
                f"{identifier}: invalid size or alignment"
            )
        offset = align_up(offset, item_alignment)
        slots.append(
            Slot(
                identifier=identifier,
                name=candidate["name"],
                canonical_type=candidate["canonical_type"],
                enum_name=slot_enum(identifier),
                offset=offset,
                size=item_size,
                alignment=item_alignment,
                copy_template=bool(candidate["has_initializer"]),
            )
        )
        offset += item_size
        maximum_alignment = max(maximum_alignment, item_alignment)
    by_id = {slot.identifier: slot for slot in slots}
    relocations = alignment.get("relocations")
    if not isinstance(relocations, list):
        raise FrontendToolStatePolicyError(
            "alignment report: relocations must be an array"
        )
    for entry in relocations:
        if not isinstance(entry, dict):
            raise FrontendToolStatePolicyError(
                "alignment report: invalid relocation"
            )
        owner = entry.get("owner_id")
        target = entry.get("target_id")
        if owner in by_id and target in by_id:
            by_id[owner].relocations.append(target)
        elif owner in by_id or target in by_id:
            raise FrontendToolStatePolicyError(
                f"cross-owner tool relocation is unsupported: {owner!r} -> {target!r}"
            )
    for slot in slots:
        slot.relocations.sort()
    initializer_uses: set[tuple[str, int, int, str, str]] = set()
    for use in catalog.get("uses", []):
        if not isinstance(use, dict) or not use.get("in_static_initializer"):
            continue
        owner = use.get("static_initializer_owner_id")
        target = use.get("id")
        path = use.get("path")
        offset_value = use.get("offset")
        length = use.get("length")
        if (
            owner not in by_id
            or target not in by_id
            or target not in by_id[owner].relocations
        ):
            continue
        if (
            not isinstance(path, str)
            or not isinstance(offset_value, int)
            or not isinstance(length, int)
        ):
            raise FrontendToolStatePolicyError(
                f"invalid relocation use for {owner!r} -> {target!r}"
            )
        initializer_uses.add((path, offset_value, length, owner, target))
    for _path, _offset, _length, owner, target in initializer_uses:
        counts = by_id[owner].relocation_counts
        counts[target] = counts.get(target, 0) + 1
    for slot in slots:
        if set(slot.relocation_counts) != set(slot.relocations):
            missing = sorted(set(slot.relocations) - set(slot.relocation_counts))
            raise FrontendToolStatePolicyError(
                f"relocation edges have no initializer uses for {slot.identifier}: "
                + ", ".join(missing)
            )
    return slots, align_up(offset, maximum_alignment), maximum_alignment


def emit_layout(slots: list[Slot], byte_count: int, alignment: int) -> str:
    lines = [
        "/* Generated PostgreSQL-dependent frontend-tool state facts. */",
        "#ifndef POSTGAMMA_TOOL_STATE_LAYOUT_H",
        "#define POSTGAMMA_TOOL_STATE_LAYOUT_H",
        "",
        "typedef enum PostgammaToolStateSlot",
        "{",
    ]
    for index, slot in enumerate(slots):
        lines.append(f"\t{slot.enum_name} = {index},")
    lines.extend(
        [
            f"\tPOSTGAMMA_TOOL_STATE_SLOT_COUNT = {len(slots)}",
            "} PostgammaToolStateSlot;",
            "",
            f"#define POSTGAMMA_TOOL_STATE_BYTES {byte_count}",
            f"#define POSTGAMMA_TOOL_STATE_ALIGNMENT {alignment}",
            "",
        ]
    )
    for slot in slots:
        if slot.relocations:
            lines.append(f"void *{relocation_bridge_name(slot.identifier)}(void);")
    if any(slot.relocations for slot in slots):
        lines.append("")
    external_slots = [
        slot for slot in slots if slot.identifier == f"external:{slot.name}"
    ]
    for slot in external_slots:
        if re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", slot.name) is None:
            raise FrontendToolStatePolicyError(
                f"cannot emit an external tool-state accessor for {slot.name!r}"
            )
        lines.extend(
            [
                f"#define POSTGAMMA_TOOL_STATE_GLOBAL_{slot.name} \\",
                f"\tPOSTGAMMA_TOOL_STATE_VALUE({slot.enum_name}, {slot.name})",
            ]
        )
    if external_slots:
        lines.extend(
            [
                "#define POSTGAMMA_TOOL_STATE_GLOBAL_EXPAND(name) \\",
                "\tPOSTGAMMA_TOOL_STATE_GLOBAL_##name",
                "#define POSTGAMMA_TOOL_STATE_GLOBAL(name) \\",
                "\tPOSTGAMMA_TOOL_STATE_GLOBAL_EXPAND(name)",
                "",
            ]
        )
    lines.extend(["#endif /* POSTGAMMA_TOOL_STATE_LAYOUT_H */", ""])
    return "\n".join(lines)


def emit_descriptors(slots: list[Slot]) -> str:
    lines = ["/* Generated PostgreSQL frontend-tool state descriptor data. */"]
    for slot in slots:
        identifier = json.dumps(slot.identifier, ensure_ascii=True)
        copy_template = "true" if slot.copy_template else "false"
        has_relocations = "true" if slot.relocations else "false"
        lines.append(
            f"\t{{{identifier}, {slot.offset}, {slot.size}, {slot.alignment}, "
            f"{copy_template}, {has_relocations}}},"
        )
    lines.append("")
    return "\n".join(lines)


def write_if_changed(path: Path, content: str) -> None:
    data = content.encode("utf-8")
    if path.is_file() and path.read_bytes() == data:
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_bytes(data)
    temporary.replace(path)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--catalog", required=True, type=Path)
    parser.add_argument("--policy", required=True, type=Path)
    parser.add_argument("--alignment", required=True, type=Path)
    parser.add_argument("--api-header", required=True, type=Path)
    parser.add_argument("--output-dir", required=True, type=Path)
    parser.add_argument("--stamp", required=True, type=Path)
    args = parser.parse_args()
    try:
        catalog = load_json(args.catalog, FrontendToolStatePolicyError)
        policy = load_json(args.policy, FrontendToolStatePolicyError)
        alignment = load_json(args.alignment, FrontendToolStatePolicyError)
        expected = validate_alignment(catalog, policy)
        if alignment != expected:
            raise FrontendToolStatePolicyError(
                "alignment report is stale; rerun frontend-tool-state-policy-check"
            )
        slots, byte_count, maximum_alignment = compile_slots(
            catalog, policy, alignment
        )
        api = args.api_header.read_text(encoding="utf-8")
    except (OSError, KeyError, TypeError, FrontendToolStatePolicyError) as exc:
        parser.error(str(exc))

    outputs = {
        "include/postgamma/tool_state_runtime.h": api,
        "include/postgamma/tool_state_layout.h": emit_layout(
            slots, byte_count, maximum_alignment
        ),
        "include/postgamma/tool_state_descriptors.inc": emit_descriptors(slots),
    }
    for relative, content in sorted(outputs.items()):
        write_if_changed(args.output_dir / relative, content)
    metadata = {
        "schema_version": 1,
        "kind": RUNTIME_KIND,
        "outputs": {
            relative: hashlib.sha256(content.encode("utf-8")).hexdigest()
            for relative, content in sorted(outputs.items())
        },
        "slot_count": len(slots),
        "byte_count": byte_count,
        "alignment": maximum_alignment,
        "slots": [
            {
                "id": slot.identifier,
                "name": slot.name,
                "canonical_type": slot.canonical_type,
                "enum": slot.enum_name,
                "offset": slot.offset,
                "size": slot.size,
                "alignment": slot.alignment,
                "copy_template": slot.copy_template,
                "relocations": slot.relocations,
                "relocation_counts": slot.relocation_counts,
            }
            for slot in slots
        ],
    }
    write_if_changed(
        args.stamp, json.dumps(metadata, indent=2, sort_keys=True) + "\n"
    )
    args.stamp.touch()
    print(
        f"frontend-tool state layout: {len(slots)} slots, {byte_count} bytes"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
