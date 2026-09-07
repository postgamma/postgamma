#!/usr/bin/env python3
"""Generate PostgreSQL-dependent backend-state layout facts."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

from check_backend_state_policy import (
    VIRTUAL_OWNERS,
    catalog_candidates,
    load_json,
    policy_decisions,
    validate_alignment,
)


@dataclass
class Slot:
    identifier: str
    name: str
    canonical_type: str
    owner: str
    enum_name: str
    index: int
    local_index: int
    offset: int
    size: int
    alignment: int
    copy_template: bool
    relocations: list[str] = field(default_factory=list)


def align_up(value: int, alignment: int) -> int:
    return (value + alignment - 1) // alignment * alignment


def enum_name(identifier: str) -> str:
    digest = hashlib.sha256(identifier.encode("utf-8")).hexdigest()[:16].upper()
    return f"POSTGAMMA_BACKEND_STATE_SLOT_{digest}"


def relocation_bridge_name(identifier: str) -> str:
    digest = hashlib.sha256(identifier.encode("utf-8")).hexdigest()[:16]
    return f"postgamma_backend_state_relocated_{digest}"


def compile_slots(
    catalog: dict[str, Any], policy: dict[str, Any], alignment: dict[str, Any]
) -> tuple[list[Slot], dict[str, int], dict[str, int]]:
    candidates = catalog_candidates(catalog)
    decisions = policy_decisions(policy)
    virtual = [
        (identifier, candidates[identifier], decisions[identifier]["owner"])
        for identifier in sorted(candidates)
        if decisions[identifier]["owner"] in VIRTUAL_OWNERS
    ]
    offsets = {owner: 0 for owner in VIRTUAL_OWNERS}
    counts = {owner: 0 for owner in VIRTUAL_OWNERS}
    slots: list[Slot] = []
    seen_enums: set[str] = set()
    for index, (identifier, candidate, owner) in enumerate(virtual):
        slot_enum = enum_name(identifier)
        if slot_enum in seen_enums:
            raise ValueError(f"generated slot collision for {identifier}")
        seen_enums.add(slot_enum)
        item_alignment = int(candidate["alignment"])
        offset = align_up(offsets[owner], item_alignment)
        slots.append(
            Slot(
                identifier=identifier,
                name=candidate["name"],
                canonical_type=candidate["canonical_type"],
                owner=owner,
                enum_name=slot_enum,
                index=index,
                local_index=counts[owner],
                offset=offset,
                size=int(candidate["size"]),
                alignment=item_alignment,
                copy_template=bool(candidate["has_initializer"]),
            )
        )
        offsets[owner] = offset + int(candidate["size"])
        counts[owner] += 1

    by_id = {slot.identifier: slot for slot in slots}
    raw_relocations = alignment.get("relocations")
    if not isinstance(raw_relocations, list):
        raise ValueError("alignment report: relocations must be an array")
    for entry in raw_relocations:
        if not isinstance(entry, dict):
            raise ValueError("alignment report: invalid relocation")
        owner_id = entry.get("owner_id")
        target_id = entry.get("target_id")
        if owner_id not in by_id or target_id not in by_id:
            raise ValueError(
                f"alignment report: unknown relocation {owner_id!r} -> {target_id!r}"
            )
        by_id[owner_id].relocations.append(target_id)
    for slot in slots:
        slot.relocations.sort()
    for owner in offsets:
        offsets[owner] = align_up(offsets[owner], 8) if offsets[owner] else 0
    return slots, offsets, counts


def emit_layout_header(
    slots: list[Slot], sizes: dict[str, int], counts: dict[str, int]
) -> str:
    """Emit declarations and constants, never runtime function bodies."""

    lines = [
        "/* Generated PostgreSQL-dependent backend-state layout facts. */",
        "#ifndef POSTGAMMA_BACKEND_STATE_LAYOUT_H",
        "#define POSTGAMMA_BACKEND_STATE_LAYOUT_H",
        "",
        "typedef enum PostgammaBackendStateSlot",
        "{",
    ]
    for slot in slots:
        lines.append(f"\t{slot.enum_name} = {slot.index},")
    lines.extend(
        [
            f"\tPOSTGAMMA_BACKEND_STATE_SLOT_COUNT = {len(slots)}",
            "} PostgammaBackendStateSlot;",
            "",
            f"#define POSTGAMMA_BACKEND_INSTANCE_STATE_BYTES {sizes['instance']}",
            f"#define POSTGAMMA_BACKEND_INSTANCE_STATE_SLOTS {counts['instance']}",
            f"#define POSTGAMMA_BACKEND_ROLE_STATE_BYTES {sizes['role']}",
            f"#define POSTGAMMA_BACKEND_ROLE_STATE_SLOTS {counts['role']}",
            f"#define POSTGAMMA_BACKEND_SESSION_STATE_BYTES {sizes['session']}",
            f"#define POSTGAMMA_BACKEND_SESSION_STATE_SLOTS {counts['session']}",
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
            raise ValueError(
                f"cannot emit an external-state accessor for {slot.name!r}"
            )
        lines.extend(
            [
                f"#define POSTGAMMA_BACKEND_STATE_GLOBAL_{slot.name} \\",
                f"\tPOSTGAMMA_BACKEND_STATE_VALUE({slot.enum_name}, {slot.name})",
            ]
        )
    if external_slots:
        lines.extend(
            [
                "#define POSTGAMMA_BACKEND_STATE_GLOBAL_EXPAND(name) \\",
                "\tPOSTGAMMA_BACKEND_STATE_GLOBAL_##name",
                "#define POSTGAMMA_BACKEND_STATE_GLOBAL(name) \\",
                "\tPOSTGAMMA_BACKEND_STATE_GLOBAL_EXPAND(name)",
                "",
            ]
        )
    lines.extend(
        [
            "#endif /* POSTGAMMA_BACKEND_STATE_LAYOUT_H */",
            "",
        ]
    )
    return "\n".join(lines)


def emit_descriptor_initializers(slots: list[Slot]) -> str:
    """Emit data rows consumed by the handwritten C runtime."""

    owner_enum = {
        "instance": "POSTGAMMA_BACKEND_STATE_OWNER_INSTANCE",
        "role": "POSTGAMMA_BACKEND_STATE_OWNER_ROLE",
        "session": "POSTGAMMA_BACKEND_STATE_OWNER_SESSION",
    }
    lines = [
        "/* Generated PostgreSQL-dependent backend-state descriptor data. */",
    ]
    for slot in slots:
        copy_template = "true" if slot.copy_template else "false"
        has_relocations = "true" if slot.relocations else "false"
        identifier = json.dumps(slot.identifier, ensure_ascii=True)
        lines.append(
            f"\t{{{identifier}, {owner_enum[slot.owner]}, {slot.local_index}, "
            f"{slot.offset}, {slot.size}, {slot.alignment}, {copy_template}, "
            f"{has_relocations}}},"
        )
    lines.append("")
    return "\n".join(lines)


def write_if_changed(path: Path, content: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.exists() and path.read_text(encoding="utf-8") == content:
        return
    path.write_text(content, encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--catalog", required=True, type=Path)
    parser.add_argument("--policy", required=True, type=Path)
    parser.add_argument("--review-rules", required=True, type=Path)
    parser.add_argument("--guc-inventory", required=True, type=Path)
    parser.add_argument("--alignment", required=True, type=Path)
    parser.add_argument("--api-header", required=True, type=Path)
    parser.add_argument("--output-dir", required=True, type=Path)
    parser.add_argument("--stamp", required=True, type=Path)
    args = parser.parse_args()

    catalog = load_json(args.catalog)
    policy = load_json(args.policy)
    review_rules = load_json(args.review_rules)
    guc_inventory = load_json(args.guc_inventory)
    alignment = load_json(args.alignment)
    try:
        expected_alignment = validate_alignment(
            catalog, policy, guc_inventory, review_rules
        )
        if alignment != expected_alignment:
            raise ValueError("alignment report is stale; rerun backend-state-policy-check")
        slots, sizes, counts = compile_slots(catalog, policy, alignment)
        api_header_content = args.api_header.read_text(encoding="utf-8")
    except (OSError, ValueError, KeyError, TypeError) as exc:
        parser.error(str(exc))

    api_header = args.output_dir / "include/postgamma/backend_state_runtime.h"
    layout_header = args.output_dir / "include/postgamma/backend_state_layout.h"
    descriptor_data = (
        args.output_dir / "include/postgamma/backend_state_descriptors.inc"
    )
    outputs = {
        "include/postgamma/backend_state_runtime.h": api_header_content,
        "include/postgamma/backend_state_layout.h": emit_layout_header(
            slots, sizes, counts
        ),
        "include/postgamma/backend_state_descriptors.inc": (
            emit_descriptor_initializers(slots)
        ),
    }
    write_if_changed(api_header, outputs["include/postgamma/backend_state_runtime.h"])
    write_if_changed(layout_header, outputs["include/postgamma/backend_state_layout.h"])
    write_if_changed(
        descriptor_data,
        outputs["include/postgamma/backend_state_descriptors.inc"],
    )

    obsolete_source = args.output_dir / "src/postgres_backend_state.c"
    if obsolete_source.is_file():
        obsolete_source.unlink()

    metadata = {
        "schema_version": 1,
        "kind": "postgamma.backend-state-runtime",
        "outputs": {
            relative: hashlib.sha256(content.encode("utf-8")).hexdigest()
            for relative, content in sorted(outputs.items())
        },
        "slot_count": len(slots),
        "owner_bytes": sizes,
        "owner_slots": counts,
        "slots": [
            {
                "id": slot.identifier,
                "name": slot.name,
                "canonical_type": slot.canonical_type,
                "owner": slot.owner,
                "enum": slot.enum_name,
                "offset": slot.offset,
                "size": slot.size,
                "alignment": slot.alignment,
                "copy_template": slot.copy_template,
                "relocations": slot.relocations,
            }
            for slot in slots
        ],
    }
    write_if_changed(args.stamp, json.dumps(metadata, indent=2, sort_keys=True) + "\n")
    args.stamp.touch()
    print(
        f"backend state layout: {len(slots)} slots, "
        f"instance={sizes['instance']} bytes, role={sizes['role']} bytes, "
        f"session={sizes['session']} bytes"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
