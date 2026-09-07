#!/usr/bin/env python3
"""Validate static-extension policy and emit immutable C initializer facts."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import tempfile
from collections import Counter
from pathlib import Path, PurePosixPath
from typing import Any


MANIFEST_KIND = "postgamma.static-extension-manifest"
REPORT_KIND = "postgamma.static-module-facts"
SIGNATURES = frozenset({"int", "pg_finfo", "pg_function", "pg_magic", "void"})
C_IDENTIFIER = re.compile(r"[A-Za-z_][A-Za-z0-9_]*\Z")
MODULE_ID = re.compile(r"[a-z][a-z0-9_]*\Z")
LOGICAL_MODULE_NAME = re.compile(r"[A-Za-z0-9][A-Za-z0-9_.-]*\Z")
CAPABILITY = re.compile(r"[a-z][a-z0-9]*(?:-[a-z0-9]+)*\Z")


class StaticModuleFactsError(ValueError):
    """The static-extension policy cannot produce safe immutable facts."""


def _unique_object(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    counts = Counter(key for key, _value in pairs)
    duplicates = sorted(key for key, count in counts.items() if count > 1)
    if duplicates:
        raise StaticModuleFactsError(
            "duplicate JSON key(s): " + ", ".join(duplicates)
        )
    return dict(pairs)


def load_json(path: Path) -> dict[str, Any]:
    try:
        document = json.loads(
            path.read_text(encoding="utf-8"), object_pairs_hook=_unique_object
        )
    except (OSError, json.JSONDecodeError, StaticModuleFactsError) as exc:
        raise StaticModuleFactsError(f"cannot read {path}: {exc}") from exc
    if not isinstance(document, dict):
        raise StaticModuleFactsError(f"{path}: top-level value must be an object")
    return document


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def _reject_unknown(value: dict[str, Any], allowed: set[str], label: str) -> None:
    unknown = sorted(set(value) - allowed)
    if unknown:
        raise StaticModuleFactsError(
            f"{label} has unknown field(s): " + ", ".join(unknown)
        )


def _string(value: Any, label: str) -> str:
    if not isinstance(value, str) or not value:
        raise StaticModuleFactsError(f"{label} must be a non-empty string")
    return value


def _relative_path(value: Any, label: str) -> str:
    result = _string(value, label)
    path = PurePosixPath(result)
    if (
        path.is_absolute()
        or "." in path.parts
        or ".." in path.parts
        or "\\" in result
    ):
        raise StaticModuleFactsError(f"{label} must be a safe relative path")
    return result


def _string_array(value: Any, label: str) -> list[str]:
    if not isinstance(value, list) or not all(
        isinstance(item, str) and item for item in value
    ):
        raise StaticModuleFactsError(f"{label} must be an array of non-empty strings")
    if len(value) != len(set(value)):
        raise StaticModuleFactsError(f"{label} contains duplicate values")
    return sorted(value)


def _require_resources(
    project_root: Path, module: dict[str, Any], label: str
) -> dict[str, str]:
    resources: dict[str, str] = {}
    for field in ("source", "header", "control", "sql"):
        relative = _relative_path(module.get(field), f"{label}.{field}")
        path = (project_root / Path(*PurePosixPath(relative).parts)).resolve()
        try:
            path.relative_to(project_root)
        except ValueError as exc:
            raise StaticModuleFactsError(
                f"{label}.{field} escapes the project root: {relative}"
            ) from exc
        if not path.is_file():
            raise StaticModuleFactsError(f"{label}.{field} does not exist: {relative}")
        resources[field] = relative
    return resources


def validate_manifest(
    document: dict[str, Any], project_root: Path
) -> dict[str, Any]:
    project_root = project_root.resolve()
    _reject_unknown(
        document,
        {
            "schema_version",
            "kind",
            "id",
            "public_symbols",
            "required_hidden_kernel_symbols",
            "forbidden_dynamic_symbols",
            "unregistered_linker_symbols",
            "modules",
        },
        "static extension manifest",
    )
    if document.get("schema_version") != 1 or document.get("kind") != MANIFEST_KIND:
        raise StaticModuleFactsError(
            f"manifest must use schema_version 1 and kind {MANIFEST_KIND}"
        )
    identifier = _string(document.get("id"), "manifest.id")
    top_arrays: dict[str, list[str]] = {}
    for field in (
        "public_symbols",
        "required_hidden_kernel_symbols",
        "forbidden_dynamic_symbols",
        "unregistered_linker_symbols",
    ):
        values = _string_array(document.get(field), f"manifest.{field}")
        if not values:
            raise StaticModuleFactsError(f"manifest.{field} must not be empty")
        if any(not C_IDENTIFIER.fullmatch(value) for value in values):
            raise StaticModuleFactsError(
                f"manifest.{field} must contain C linker identifiers"
            )
        top_arrays[field] = values
    if any(not symbol.startswith("pgm_") for symbol in top_arrays["public_symbols"]):
        raise StaticModuleFactsError("manifest public symbols must use the pgm_ prefix")

    raw_modules = document.get("modules")
    if not isinstance(raw_modules, list) or not raw_modules:
        raise StaticModuleFactsError("manifest.modules must be a non-empty array")
    modules: list[dict[str, Any]] = []
    ids: set[str] = set()
    logical_names: set[str] = set()
    linker_names: set[str] = set()
    for index, raw_module in enumerate(raw_modules):
        label = f"manifest.modules[{index}]"
        if not isinstance(raw_module, dict):
            raise StaticModuleFactsError(f"{label} must be an object")
        _reject_unknown(
            raw_module,
            {
                "id",
                "logical_name",
                "source",
                "header",
                "control",
                "sql",
                "capabilities",
                "symbols",
            },
            label,
        )
        module_id = _string(raw_module.get("id"), f"{label}.id")
        if not MODULE_ID.fullmatch(module_id) or module_id in ids:
            raise StaticModuleFactsError(f"{label}.id is invalid or duplicated")
        ids.add(module_id)
        logical_name = _string(
            raw_module.get("logical_name"), f"{label}.logical_name"
        )
        if (
            not LOGICAL_MODULE_NAME.fullmatch(logical_name)
            or logical_name in logical_names
        ):
            raise StaticModuleFactsError(
                f"{label}.logical_name is invalid or duplicated"
            )
        logical_names.add(logical_name)
        resources = _require_resources(project_root, raw_module, label)
        capabilities = _string_array(
            raw_module.get("capabilities"), f"{label}.capabilities"
        )
        if not capabilities:
            raise StaticModuleFactsError(f"{label}.capabilities must not be empty")
        if any(not CAPABILITY.fullmatch(value) for value in capabilities):
            raise StaticModuleFactsError(f"{label}.capabilities contains an invalid name")

        raw_symbols = raw_module.get("symbols")
        if not isinstance(raw_symbols, list) or not raw_symbols:
            raise StaticModuleFactsError(f"{label}.symbols must be a non-empty array")
        symbols: list[dict[str, str]] = []
        module_logical_symbols: set[str] = set()
        for symbol_index, raw_symbol in enumerate(raw_symbols):
            symbol_label = f"{label}.symbols[{symbol_index}]"
            if not isinstance(raw_symbol, dict):
                raise StaticModuleFactsError(f"{symbol_label} must be an object")
            _reject_unknown(
                raw_symbol,
                {"logical_name", "linker_name", "signature"},
                symbol_label,
            )
            symbol_logical_name = _string(
                raw_symbol.get("logical_name"), f"{symbol_label}.logical_name"
            )
            linker_name = _string(
                raw_symbol.get("linker_name"), f"{symbol_label}.linker_name"
            )
            signature = raw_symbol.get("signature")
            if not C_IDENTIFIER.fullmatch(symbol_logical_name):
                raise StaticModuleFactsError(
                    f"{symbol_label}.logical_name is not a C symbol name"
                )
            if not C_IDENTIFIER.fullmatch(linker_name):
                raise StaticModuleFactsError(
                    f"{symbol_label}.linker_name is not a C identifier"
                )
            if signature not in SIGNATURES:
                raise StaticModuleFactsError(f"{symbol_label}.signature is invalid")
            if symbol_logical_name in module_logical_symbols:
                raise StaticModuleFactsError(
                    f"{label} contains duplicate logical symbol {symbol_logical_name}"
                )
            if linker_name in linker_names:
                raise StaticModuleFactsError(
                    f"manifest contains duplicate linker symbol {linker_name}"
                )
            module_logical_symbols.add(symbol_logical_name)
            linker_names.add(linker_name)
            symbols.append(
                {
                    "logical_name": symbol_logical_name,
                    "linker_name": linker_name,
                    "signature": signature,
                }
            )
        by_logical = {symbol["logical_name"]: symbol for symbol in symbols}
        if by_logical.get("Pg_magic_func", {}).get("signature") != "pg_magic":
            raise StaticModuleFactsError(
                f"{label} must declare Pg_magic_func with pg_magic signature"
            )
        if "_PG_init" in by_logical and by_logical["_PG_init"]["signature"] != "void":
            raise StaticModuleFactsError(f"{label}._PG_init must use void signature")
        if not any(symbol["signature"] == "pg_function" for symbol in symbols):
            raise StaticModuleFactsError(f"{label} must declare a PostgreSQL function")
        modules.append(
            {
                "id": module_id,
                "logical_name": logical_name,
                **resources,
                "capabilities": capabilities,
                "symbols": sorted(symbols, key=lambda item: item["logical_name"]),
            }
        )
    return {
        "schema_version": 1,
        "kind": MANIFEST_KIND,
        "id": identifier,
        **top_arrays,
        "modules": sorted(modules, key=lambda module: module["logical_name"]),
    }


def c_string(value: str) -> str:
    return json.dumps(value, ensure_ascii=True)


def render_facts(manifest: dict[str, Any]) -> str:
    lines = [
        "/* Generated static-module initializer facts.  Do not edit. */",
        "",
    ]
    for module in manifest["modules"]:
        lines.append(f'#include {c_string(module["header"])}')
    lines.append("")
    for module in manifest["modules"]:
        array = f'postgamma_generated_{module["id"]}_symbols'
        lines.append(f"static const PostgammaStaticModuleSymbol {array}[] =")
        lines.append("{")
        for symbol in module["symbols"]:
            lines.append(
                "\t{"
                + c_string(symbol["logical_name"])
                + ", POSTGAMMA_STATIC_FUNCTION("
                + symbol["linker_name"]
                + ")},"
            )
        lines.extend(("};", ""))
    lines.append(
        "static const PostgammaStaticModuleDefinition postgamma_generated_modules[] ="
    )
    lines.append("{")
    for module in manifest["modules"]:
        lines.append(
            "\tPOSTGAMMA_STATIC_MODULE_INITIALIZER("
            + c_string(module["logical_name"])
            + f', postgamma_generated_{module["id"]}_symbols),'
        )
    lines.extend(
        (
            "};",
            "",
            "static const PostgammaStaticModuleRegistry postgamma_generated_registry =",
            "\tPOSTGAMMA_STATIC_REGISTRY_INITIALIZER(postgamma_generated_modules);",
            "",
        )
    )
    return "\n".join(lines)


def facts_report(
    manifest: dict[str, Any], manifest_path: Path, project_root: Path
) -> dict[str, Any]:
    resources: dict[str, str] = {}
    for module in manifest["modules"]:
        for field in ("source", "header", "control", "sql"):
            relative = module[field]
            resources[relative] = sha256(project_root / relative)
    return {
        "schema_version": 1,
        "kind": REPORT_KIND,
        "manifest_id": manifest["id"],
        "manifest_sha256": sha256(manifest_path),
        "module_count": len(manifest["modules"]),
        "symbol_count": sum(len(module["symbols"]) for module in manifest["modules"]),
        "resources": dict(sorted(resources.items())),
    }


def write_text(path: Path, content: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.exists() and path.read_text(encoding="utf-8") == content:
        os.utime(path, None)
        return
    with tempfile.NamedTemporaryFile(
        mode="w", encoding="utf-8", dir=path.parent, prefix=f".{path.name}.", delete=False
    ) as handle:
        handle.write(content)
        temporary = Path(handle.name)
    try:
        os.replace(temporary, path)
    finally:
        temporary.unlink(missing_ok=True)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--manifest", required=True, type=Path)
    parser.add_argument("--project-root", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--report", required=True, type=Path)
    args = parser.parse_args()
    try:
        manifest_path = args.manifest.resolve()
        project_root = args.project_root.resolve()
        manifest = validate_manifest(load_json(manifest_path), project_root)
        write_text(args.output.resolve(), render_facts(manifest))
        write_text(
            args.report.resolve(),
            json.dumps(
                facts_report(manifest, manifest_path, project_root),
                indent=2,
                sort_keys=True,
            )
            + "\n",
        )
    except (OSError, StaticModuleFactsError) as exc:
        parser.error(str(exc))
    print(
        f"static module facts: {len(manifest['modules'])} module(s), "
        f"{sum(len(module['symbols']) for module in manifest['modules'])} symbol(s) "
        f"-> {args.output}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
