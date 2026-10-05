#!/usr/bin/env python3
"""Generate PostgreSQL-dependent GUC state and descriptor facts."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
from pathlib import Path
from typing import Any

from check_guc_policy import (
    PolicyError,
    catalog_names,
    load_json,
    policy_owners,
    validate_alignment,
)


CATALOG_KIND = "postgamma.postgres-guc-catalog"
CONTROL_KIND = "postgamma.guc-control-ownership"
OWNERS = ("immutable", "instance", "role", "session")
VALUE_C_TYPES = {
    "bool": "bool",
    "int": "int",
    "real": "double",
    "string": "char *",
    "enum": "int",
}
PG_VALUE_TYPES = {
    "bool": "PGC_BOOL",
    "int": "PGC_INT",
    "real": "PGC_REAL",
    "string": "PGC_STRING",
    "enum": "PGC_ENUM",
}
STATE_PATHS = {
    "immutable": "instance->immutable_guc",
    "instance": "instance->instance_guc",
    "role": "role->guc",
    "session": "session_guc",
}
IDENTIFIER = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")
CONTROL_INITIALIZERS = {"zero", "dlist", "slist", "configure_names"}


def _slot_identifier(storage: str) -> str:
    return "POSTGAMMA_GUC_SLOT_" + re.sub(r"[^A-Za-z0-9_]", "_", storage).upper()


def _validate_catalog(catalog: dict[str, Any]) -> list[dict[str, str]]:
    if catalog.get("schema_version") != 1 or catalog.get("kind") != CATALOG_KIND:
        raise PolicyError(
            f"catalog: expected schema_version 1 and kind {CATALOG_KIND}"
        )
    raw = catalog.get("parameters")
    if not isinstance(raw, list):
        raise PolicyError("catalog: parameters must be an array")
    if catalog.get("parameter_count") != len(raw):
        raise PolicyError("catalog: parameter_count does not match parameters")

    parameters: list[dict[str, str]] = []
    seen_names: set[str] = set()
    seen_storage: set[str] = set()
    seen_slots: set[str] = set()
    for index, entry in enumerate(raw):
        if not isinstance(entry, dict):
            raise PolicyError(f"catalog: parameters[{index}] must be an object")
        required = ("name", "storage_symbol", "value_type", "compile_condition")
        if any(not isinstance(entry.get(field), str) for field in required):
            raise PolicyError(f"catalog: parameters[{index}] has invalid facts")
        name = entry["name"]
        storage = entry["storage_symbol"]
        value_type = entry["value_type"]
        condition = entry["compile_condition"]
        if not name or name in seen_names:
            raise PolicyError(f"catalog: duplicate or empty GUC name {name!r}")
        if not IDENTIFIER.fullmatch(storage) or storage in seen_storage:
            raise PolicyError(f"catalog: invalid or duplicate storage symbol {storage!r}")
        if value_type not in VALUE_C_TYPES:
            raise PolicyError(f"catalog: unsupported value type {value_type!r}")
        if condition and not IDENTIFIER.fullmatch(condition):
            raise PolicyError(f"catalog: invalid compile condition {condition!r}")
        slot = _slot_identifier(storage)
        if slot in seen_slots:
            raise PolicyError(f"catalog: generated slot collision for {storage}")
        seen_names.add(name)
        seen_storage.add(storage)
        seen_slots.add(slot)
        parameters.append(
            {
                "name": name,
                "storage_symbol": storage,
                "value_type": value_type,
                "compile_condition": condition,
                "slot": slot,
            }
        )
    return sorted(parameters, key=lambda item: item["name"])


def _validate_controls(controls: dict[str, Any]) -> list[dict[str, str]]:
    if controls.get("schema_version") != 1 or controls.get("kind") != CONTROL_KIND:
        raise PolicyError(
            f"controls: expected schema_version 1 and kind {CONTROL_KIND}"
        )
    raw = controls.get("symbols")
    if not isinstance(raw, list):
        raise PolicyError("controls: symbols must be an array")
    required = (
        "id",
        "name",
        "replacement",
        "runtime_field",
        "runtime_c_type",
        "initializer",
    )
    result: list[dict[str, str]] = []
    seen_ids: set[str] = set()
    seen_names: set[str] = set()
    seen_replacements: set[str] = set()
    seen_fields: set[str] = set()
    configure_count = 0
    for index, entry in enumerate(raw):
        if not isinstance(entry, dict) or any(
            not isinstance(entry.get(field), str) or not entry[field]
            for field in required
        ):
            raise PolicyError(f"controls: symbols[{index}] has invalid runtime facts")
        if entry.get("slot") != "session":
            raise PolicyError(f"controls: symbols[{index}] must be session-owned")
        identifier = entry["id"]
        name = entry["name"]
        replacement = entry["replacement"]
        field = entry["runtime_field"]
        initializer = entry["initializer"]
        if identifier in seen_ids or name in seen_names:
            raise PolicyError(f"controls: duplicate id or name at symbols[{index}]")
        if replacement in seen_replacements or not IDENTIFIER.fullmatch(replacement):
            raise PolicyError(f"controls: invalid or duplicate replacement {replacement!r}")
        if field in seen_fields or not IDENTIFIER.fullmatch(field):
            raise PolicyError(f"controls: invalid or duplicate runtime field {field!r}")
        if initializer not in CONTROL_INITIALIZERS:
            raise PolicyError(f"controls: unsupported initializer {initializer!r}")
        configure_count += initializer == "configure_names"
        seen_ids.add(identifier)
        seen_names.add(name)
        seen_replacements.add(replacement)
        seen_fields.add(field)
        result.append({key: entry[key] for key in required})
    if configure_count != 1:
        raise PolicyError("controls: exactly one configure_names field is required")
    return sorted(result, key=lambda item: item["name"])


def _state_type_name(owner: str) -> str:
    return "Postgamma" + owner.title() + "GucState"


def _emit_state(owner: str, parameters: list[dict[str, str]]) -> list[str]:
    lines = [f"typedef struct {_state_type_name(owner)}", "{"]
    owned = [entry for entry in parameters if entry["owner"] == owner]
    for entry in owned:
        lines.append(
            f"\t{VALUE_C_TYPES[entry['value_type']]:<8}\t{entry['storage_symbol']};"
        )
    if not owned:
        lines.append("\tunsigned char unused;")
    lines.extend([f"}} {_state_type_name(owner)};", ""])
    return lines


def emit_state_layout(
    parameters: list[dict[str, str]], controls: list[dict[str, str]]
) -> str:
    """Emit typed fields, enums, and access macros without function bodies."""

    lines = [
        "/* Generated PostgreSQL-dependent GUC state layout facts. */",
        "#ifndef POSTGAMMA_GUC_STATE_LAYOUT_H",
        "#define POSTGAMMA_GUC_STATE_LAYOUT_H",
        "",
    ]
    for owner in OWNERS:
        lines.extend(_emit_state(owner, parameters))
    lines.extend(
        [
            "typedef enum PostgammaGucSlot",
            "{",
        ]
    )
    for index, entry in enumerate(parameters):
        lines.append(f"\t{entry['slot']} = {index},")
    lines.extend(
        [
            f"\tPOSTGAMMA_GUC_SLOT_COUNT = {len(parameters)}",
            "} PostgammaGucSlot;",
            "",
            "typedef enum PostgammaGucControlSlot",
            "{",
        ]
    )
    for index, entry in enumerate(controls):
        slot = f"POSTGAMMA_GUC_CONTROL_SLOT_{entry['runtime_field'].upper()}"
        lines.append(f"\t{slot} = {index},")
    lines.extend(
        [
            f"\tPOSTGAMMA_GUC_CONTROL_SLOT_COUNT = {len(controls)}",
            "} PostgammaGucControlSlot;",
            "",
        ]
    )
    configure = next(
        entry for entry in controls if entry["initializer"] == "configure_names"
    )
    configure_slot = (
        f"POSTGAMMA_GUC_CONTROL_SLOT_{configure['runtime_field'].upper()}"
    )
    lines.extend(
        [
            f"#define POSTGAMMA_GUC_CONFIGURE_NAMES_SLOT {configure_slot}",
            "",
        ]
    )
    for entry in parameters:
        path = STATE_PATHS[entry["owner"]]
        lines.extend(
            [
                f"#define POSTGAMMA_GUC_VALUE_{entry['storage_symbol']} \\",
                f"\t(postgamma_execution_context_require()->{path}.{entry['storage_symbol']})",
            ]
        )
    lines.extend(
        [
            "#define POSTGAMMA_GUC_VALUE_EXPAND(name) POSTGAMMA_GUC_VALUE_##name",
            "#define POSTGAMMA_GUC_VALUE(name) POSTGAMMA_GUC_VALUE_EXPAND(name)",
            "",
        ]
    )
    for entry in controls:
        slot = f"POSTGAMMA_GUC_CONTROL_SLOT_{entry['runtime_field'].upper()}"
        lines.extend(
            [
                f"#define {entry['replacement']} \\",
                f"\t(*({entry['runtime_c_type']} *) "
                f"postgamma_guc_control_slot_address({slot}))",
            ]
        )
    lines.extend(
        [
            "",
            "#endif /* POSTGAMMA_GUC_STATE_LAYOUT_H */",
            "",
        ]
    )
    return "\n".join(lines)


def emit_descriptor_facts(parameters: list[dict[str, str]]) -> str:
    lines = ["/* Generated PostgreSQL built-in GUC descriptor facts. */"]
    for entry in parameters:
        condition = entry["compile_condition"]
        if condition:
            lines.append(f"#ifdef {condition}")
        name = json.dumps(entry["name"], ensure_ascii=True)
        lines.append(
            f"POSTGAMMA_GUC_DESCRIPTOR({name}, {entry['slot']}, "
            f"{PG_VALUE_TYPES[entry['value_type']]}, {entry['storage_symbol']}, "
            f"POSTGAMMA_GUC_OWNER_{entry['owner'].upper()})"
        )
        if condition:
            lines.append(f"#endif /* {condition} */")
    lines.append("")
    return "\n".join(lines)


def emit_control_facts(controls: list[dict[str, str]]) -> str:
    lines = ["/* Generated PostgreSQL GUC control-state facts. */"]
    for entry in controls:
        slot = f"POSTGAMMA_GUC_CONTROL_SLOT_{entry['runtime_field'].upper()}"
        lines.append(
            f"POSTGAMMA_GUC_CONTROL({slot}, {entry['runtime_field']}, "
            f"{entry['runtime_c_type']}, {entry['replacement']}, "
            f"{entry['initializer']})"
        )
    lines.append("")
    return "\n".join(lines)


def compile_runtime(
    catalog: dict[str, Any], policy: dict[str, Any], controls: dict[str, Any]
) -> dict[str, str]:
    parameters = _validate_catalog(catalog)
    owners = policy_owners(policy)
    validate_alignment(catalog_names(catalog), owners)
    for entry in parameters:
        owner = owners[entry["name"]]
        if owner not in OWNERS:
            raise PolicyError(f"policy: unsupported owner {owner!r}")
        entry["owner"] = owner
    control_entries = _validate_controls(controls)
    return {
        "include/postgamma/guc_state_layout.h": emit_state_layout(
            parameters, control_entries
        ),
        "include/postgamma/guc_descriptors.inc": emit_descriptor_facts(parameters),
        "include/postgamma/guc_control_facts.inc": emit_control_facts(
            control_entries
        ),
    }


def _write_if_changed(path: Path, content: str) -> None:
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
    parser.add_argument("--controls", required=True, type=Path)
    parser.add_argument("--api-header", required=True, type=Path)
    parser.add_argument("--output-dir", required=True, type=Path)
    parser.add_argument("--stamp", required=True, type=Path)
    args = parser.parse_args()
    try:
        outputs = compile_runtime(
            load_json(args.catalog), load_json(args.policy), load_json(args.controls)
        )
        outputs["include/postgamma/guc_runtime.h"] = args.api_header.read_text(
            encoding="utf-8"
        )
    except (OSError, PolicyError) as exc:
        parser.error(str(exc))

    for relative, content in sorted(outputs.items()):
        _write_if_changed(args.output_dir / relative, content)

    obsolete_source = args.output_dir / "src/postgres_guc_bridge.c"
    if obsolete_source.is_file():
        obsolete_source.unlink()

    stamp = {
        "schema_version": 1,
        "kind": "postgamma.generated-guc-runtime",
        "outputs": {
            relative: hashlib.sha256(content.encode("utf-8")).hexdigest()
            for relative, content in sorted(outputs.items())
        },
    }
    stamp_content = json.dumps(stamp, indent=2, sort_keys=True) + "\n"
    _write_if_changed(args.stamp, stamp_content)
    args.stamp.touch()
    print(f"generated GUC layout facts for {len(outputs)} artifacts")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
