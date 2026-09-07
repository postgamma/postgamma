#!/usr/bin/env python3
"""Compile generated GUC facts and human policy into an AST input manifest."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import Any

from check_guc_policy import (
    PolicyError,
    catalog_names,
    load_json,
    policy_owners,
    validate_alignment,
)
from postgresql_adapter import AdapterError, load_adapter, runtime_hooks_document


HOOK_KIND = "postgamma.runtime-hooks"
DOMAIN_KIND = "postgamma.postgres-source-domain"
CONTROL_KIND = "postgamma.guc-control-ownership"


def compile_manifest(
    catalog: dict[str, Any],
    policy: dict[str, Any],
    hooks: dict[str, Any],
    domain: dict[str, Any],
    controls: dict[str, Any],
) -> dict[str, Any]:
    names = catalog_names(catalog)
    owners = policy_owners(policy)
    validate_alignment(names, owners)
    if hooks.get("schema_version") != 1 or hooks.get("kind") != HOOK_KIND:
        raise PolicyError(f"hooks: expected schema_version 1 and kind {HOOK_KIND}")
    hook_entries = hooks.get("hooks")
    if not isinstance(hook_entries, list):
        raise PolicyError("hooks: hooks must be an array")
    assumption_entries = hooks.get("assumptions")
    if not isinstance(assumption_entries, list):
        raise PolicyError("hooks: assumptions must be an array")
    if domain.get("schema_version") != 1 or domain.get("kind") != DOMAIN_KIND:
        raise PolicyError(f"domain: expected schema_version 1 and kind {DOMAIN_KIND}")

    def string_array(field: str) -> list[str]:
        values = domain.get(field)
        if not isinstance(values, list) or not values:
            raise PolicyError(f"domain: {field} must be a non-empty array")
        if not all(isinstance(value, str) and value for value in values):
            raise PolicyError(f"domain: {field} entries must be non-empty strings")
        if len(values) != len(set(values)):
            raise PolicyError(f"domain: {field} contains duplicates")
        return sorted(values)

    translation_unit_prefixes = string_array("translation_unit_prefixes")
    translation_unit_files = string_array("translation_unit_files")
    binding_anchor_suffixes = string_array("binding_anchor_file_suffixes")
    if controls.get("schema_version") != 1 or controls.get("kind") != CONTROL_KIND:
        raise PolicyError(
            f"controls: expected schema_version 1 and kind {CONTROL_KIND}"
        )
    control_entries = controls.get("symbols")
    if not isinstance(control_entries, list):
        raise PolicyError("controls: symbols must be an array")

    symbols: list[dict[str, Any]] = []
    seen_storage: set[str] = set()
    for index, parameter in enumerate(catalog["parameters"]):
        if not isinstance(parameter, dict):
            raise PolicyError(f"catalog: parameters[{index}] must be an object")
        try:
            guc_name = parameter["name"]
            storage = parameter["storage_symbol"]
            value_type = parameter["value_type"]
            condition = parameter["compile_condition"]
        except KeyError as exc:
            raise PolicyError(
                f"catalog: parameters[{index}] missing {exc.args[0]}"
            ) from exc
        if not all(isinstance(value, str) for value in (guc_name, storage, value_type, condition)):
            raise PolicyError(f"catalog: parameters[{index}] contains a non-string fact")
        if storage in seen_storage:
            raise PolicyError(f"catalog: duplicate storage symbol {storage}")
        seen_storage.add(storage)
        symbols.append(
            {
                "id": f"pg.guc.{guc_name}",
                "name": storage,
                "c_type": value_type,
                "slot": owners[guc_name],
                "replacement": f"POSTGAMMA_GUC_VALUE({storage})",
                "optional": bool(condition),
            }
        )

    for index, control in enumerate(control_entries):
        if not isinstance(control, dict):
            raise PolicyError(f"controls: symbols[{index}] must be an object")
        required = (
            "id",
            "name",
            "c_type",
            "slot",
            "replacement",
            "declaration_file_suffixes",
        )
        if any(field not in control for field in required):
            raise PolicyError(f"controls: symbols[{index}] is incomplete")
        suffixes = control["declaration_file_suffixes"]
        if not isinstance(suffixes, list) or not suffixes or not all(
            isinstance(value, str) and value for value in suffixes
        ):
            raise PolicyError(
                f"controls: symbols[{index}].declaration_file_suffixes is invalid"
            )
        if not all(isinstance(control[field], str) for field in required[:-1]):
            raise PolicyError(f"controls: symbols[{index}] contains a non-string field")
        if control["name"] in seen_storage:
            raise PolicyError(f"controls: duplicate transformed symbol {control['name']}")
        seen_storage.add(control["name"])
        symbols.append(
            {
                **{field: control[field] for field in required},
                "binding_anchor": False,
                "optional": bool(control.get("optional", False)),
            }
        )

    return {
        "schema_version": 1,
        "kind": "postgamma.ast-transform",
        # gen_guc_tables.pl emits the authoritative pointer from each GUC
        # record to its backing variable.  The AST uses that reference to bind
        # a storage name to one canonical declaration identity, even when a
        # frontend utility happens to define a global with the same spelling.
        "binding_anchor_file_suffixes": binding_anchor_suffixes,
        "translation_unit_prefixes": translation_unit_prefixes,
        "translation_unit_files": translation_unit_files,
        "symbols": sorted(symbols, key=lambda item: item["name"]),
        "injections": hook_entries,
        "assumptions": assumption_entries,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--catalog", required=True, type=Path)
    parser.add_argument("--policy", required=True, type=Path)
    parser.add_argument("--adapter", required=True, type=Path)
    parser.add_argument("--domain", required=True, type=Path)
    parser.add_argument("--controls", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    try:
        adapter = load_adapter(args.adapter)
        document = compile_manifest(
            load_json(args.catalog),
            load_json(args.policy),
            runtime_hooks_document(adapter),
            load_json(args.domain),
            load_json(args.controls),
        )
    except (AdapterError, PolicyError) as exc:
        parser.error(str(exc))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    content = json.dumps(document, indent=2, sort_keys=True) + "\n"
    if not args.output.exists() or args.output.read_text(encoding="utf-8") != content:
        args.output.write_text(content, encoding="utf-8")
    print(
        f"GUC transform manifest: {len(document['symbols'])} symbols, "
        f"{len(document['injections'])} hooks, "
        f"{len(document['assumptions'])} assumptions -> {args.output}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
