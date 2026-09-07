#!/usr/bin/env python3
"""Build manifest-selected PostgreSQL modules into one private object."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import subprocess
import tempfile
from collections import Counter
from pathlib import Path, PurePosixPath
from typing import Any


MANIFEST_KIND = "postgamma.embedded-static-module-manifest"
LINK_DATABASE_KIND = "postgamma.link-command-database"
RECEIPT_KIND = "postgamma.embedded-static-module-bundle"
IDENTIFIER = re.compile(r"[A-Za-z_][A-Za-z0-9_]*\Z")
MODULE_ID = re.compile(r"[a-z][a-z0-9_]*\Z")
LOGICAL_NAME = re.compile(r"[A-Za-z0-9][A-Za-z0-9_.-]*\Z")
CAPABILITY_NAMES = (
    "thread-safe",
    "multi-instance-safe",
    "session-mobility-safe",
    "parallel-worker-safe",
    "instance-shmem",
    "background-worker",
    "filesystem-read",
    "filesystem-write",
    "host-library-dependency",
    "process-global-state",
)
CAPABILITY_BITS = {name: 1 << index for index, name in enumerate(CAPABILITY_NAMES)}
LIFECYCLE_BITS = {
    "library-initialize": 1 << 0,
    "instance-request": 1 << 1,
    "instance-startup": 1 << 2,
    "instance-shutdown": 1 << 3,
    "session-initialize": 1 << 4,
    "session-reset": 1 << 5,
    "session-destroy": 1 << 6,
}


class StaticModuleBundleError(RuntimeError):
    """A static module manifest or link closure is invalid."""


def unique_object(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    counts = Counter(name for name, _value in pairs)
    duplicates = sorted(name for name, count in counts.items() if count > 1)
    if duplicates:
        raise StaticModuleBundleError(
            "duplicate JSON key(s): " + ", ".join(duplicates)
        )
    return dict(pairs)


def read_json(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(
            path.read_text(encoding="utf-8"), object_pairs_hook=unique_object
        )
    except (OSError, json.JSONDecodeError, StaticModuleBundleError) as exc:
        raise StaticModuleBundleError(f"cannot read {path}: {exc}") from exc
    if not isinstance(value, dict):
        raise StaticModuleBundleError(f"{path}: top-level value must be an object")
    return value


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(command: list[str], label: str) -> subprocess.CompletedProcess[str]:
    try:
        completed = subprocess.run(
            command,
            check=False,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
    except OSError as exc:
        raise StaticModuleBundleError(f"cannot execute {label}: {exc}") from exc
    if completed.returncode != 0:
        diagnostic = completed.stderr.strip() or completed.stdout.strip()
        raise StaticModuleBundleError(
            f"{label} failed with status {completed.returncode}: {diagnostic}"
        )
    return completed


def global_definitions(nm: str, path: Path) -> list[str]:
    output = run(
        [nm, "-P", "-g", "--defined-only", str(path)], "nm"
    ).stdout
    result: set[str] = set()
    for line in output.splitlines():
        fields = line.split()
        if not fields:
            continue
        name = fields[0]
        if name.endswith(":") or name == "_GLOBAL_OFFSET_TABLE_":
            continue
        if not IDENTIFIER.fullmatch(name):
            raise StaticModuleBundleError(f"nm returned unsafe symbol {name!r}")
        result.add(name)
    return sorted(result)


def safe_relative_path(value: Any, label: str) -> str:
    if not isinstance(value, str) or not value:
        raise StaticModuleBundleError(f"{label} is invalid")
    path = PurePosixPath(value)
    components = value.split("/")
    if (
        path.is_absolute()
        or any(component in {"", ".", ".."} for component in components)
        or "\\" in value
        or any(
            ord(character) < 0x20 or ord(character) == 0x7F
            for character in value
        )
    ):
        raise StaticModuleBundleError(f"{label} is not a safe relative path")
    return value


def c_string_literal(value: str, label: str) -> str:
    if not isinstance(value, str) or any(
        ord(character) < 0x20 or ord(character) == 0x7F
        for character in value
    ):
        raise StaticModuleBundleError(f"{label} is not a safe C string")
    return '"' + value.replace("\\", "\\\\").replace('"', '\\"') + '"'


def _validate_symbols(
    raw_symbols: Any,
    label: str,
    linker_names: set[str],
) -> list[dict[str, str]]:
    if not isinstance(raw_symbols, list) or not raw_symbols:
        raise StaticModuleBundleError(f"{label}.symbols is empty")
    symbols: list[dict[str, str]] = []
    logical_symbols: set[str] = set()
    for symbol_index, raw_symbol in enumerate(raw_symbols):
        symbol_label = f"{label}.symbols[{symbol_index}]"
        if not isinstance(raw_symbol, dict) or set(raw_symbol) != {
            "logical_name",
            "linker_name",
        }:
            raise StaticModuleBundleError(f"{symbol_label} schema is invalid")
        logical_symbol = raw_symbol.get("logical_name")
        linker_symbol = raw_symbol.get("linker_name")
        if (
            not isinstance(logical_symbol, str)
            or not IDENTIFIER.fullmatch(logical_symbol)
            or logical_symbol in logical_symbols
        ):
            raise StaticModuleBundleError(
                f"{symbol_label}.logical_name is invalid or duplicated"
            )
        if (
            not isinstance(linker_symbol, str)
            or not IDENTIFIER.fullmatch(linker_symbol)
            or not linker_symbol.startswith("postgamma_module_")
            or linker_symbol in linker_names
        ):
            raise StaticModuleBundleError(
                f"{symbol_label}.linker_name is invalid or duplicated"
            )
        logical_symbols.add(logical_symbol)
        linker_names.add(linker_symbol)
        symbols.append(
            {"logical_name": logical_symbol, "linker_name": linker_symbol}
        )
    if "Pg_magic_func" not in logical_symbols:
        raise StaticModuleBundleError(f"{label} does not register Pg_magic_func")
    return sorted(symbols, key=lambda item: item["logical_name"])


def validate_manifest(document: dict[str, Any]) -> dict[str, Any]:
    if document.get("schema_version") == 2:
        return _validate_manifest_v2(document)
    allowed = {"schema_version", "kind", "id", "modules"}
    if set(document) != allowed:
        raise StaticModuleBundleError("static module manifest schema is invalid")
    if document.get("schema_version") != 1 or document.get("kind") != MANIFEST_KIND:
        raise StaticModuleBundleError(
            f"manifest must use schema_version 1 and kind {MANIFEST_KIND}"
        )
    if not isinstance(document.get("id"), str) or not document["id"]:
        raise StaticModuleBundleError("manifest id is invalid")
    raw_modules = document.get("modules")
    if not isinstance(raw_modules, list) or not raw_modules:
        raise StaticModuleBundleError("manifest modules must be a non-empty array")
    modules: list[dict[str, Any]] = []
    module_ids: set[str] = set()
    logical_names: set[str] = set()
    linker_names: set[str] = set()
    for index, raw_module in enumerate(raw_modules):
        label = f"manifest.modules[{index}]"
        if not isinstance(raw_module, dict) or set(raw_module) != {
            "id",
            "logical_name",
            "link_output_suffix",
            "symbols",
        }:
            raise StaticModuleBundleError(f"{label} schema is invalid")
        module_id = raw_module.get("id")
        logical_name = raw_module.get("logical_name")
        suffix = raw_module.get("link_output_suffix")
        if (
            not isinstance(module_id, str)
            or not MODULE_ID.fullmatch(module_id)
            or module_id in module_ids
        ):
            raise StaticModuleBundleError(f"{label}.id is invalid or duplicated")
        if (
            not isinstance(logical_name, str)
            or not LOGICAL_NAME.fullmatch(logical_name)
            or logical_name in logical_names
        ):
            raise StaticModuleBundleError(
                f"{label}.logical_name is invalid or duplicated"
            )
        suffix = safe_relative_path(suffix, f"{label}.link_output_suffix")
        symbols = _validate_symbols(raw_module.get("symbols"), label, linker_names)
        module_ids.add(module_id)
        logical_names.add(logical_name)
        modules.append(
            {
                "id": module_id,
                "logical_name": logical_name,
                "link_output_suffix": suffix,
                "version": "",
                "postgresql_major": 0,
                "sdk_contract": False,
                "sdk_abi_version": 0,
                "capabilities": [],
                "capability_bits": 0,
                "build": {
                    "kind": "postgres-link-output",
                    "link_output_suffix": suffix,
                },
                "state_policy": "",
                "lifecycle": [],
                "lifecycle_bits": 0,
                "resources": [],
                "dependencies": [],
                "symbols": symbols,
            }
        )
    return {
        "schema_version": 1,
        "kind": MANIFEST_KIND,
        "id": document["id"],
        "modules": sorted(modules, key=lambda item: item["logical_name"]),
    }


def _validate_manifest_v2(document: dict[str, Any]) -> dict[str, Any]:
    allowed = {
        "schema_version",
        "kind",
        "id",
        "postgresql_major",
        "capability_vocabulary",
        "modules",
    }
    if set(document) != allowed or document.get("kind") != MANIFEST_KIND:
        raise StaticModuleBundleError("static module manifest v2 schema is invalid")
    if not isinstance(document.get("id"), str) or not document["id"]:
        raise StaticModuleBundleError("manifest id is invalid")
    postgresql_major = document.get("postgresql_major")
    if not isinstance(postgresql_major, int) or postgresql_major <= 0:
        raise StaticModuleBundleError("manifest postgresql_major is invalid")
    if document.get("capability_vocabulary") != list(CAPABILITY_NAMES):
        raise StaticModuleBundleError("manifest capability vocabulary is invalid")
    raw_modules = document.get("modules")
    if not isinstance(raw_modules, list) or not raw_modules:
        raise StaticModuleBundleError("manifest modules must be a non-empty array")
    modules: list[dict[str, Any]] = []
    module_ids: set[str] = set()
    logical_names: set[str] = set()
    linker_names: set[str] = set()
    for index, raw_module in enumerate(raw_modules):
        label = f"manifest.modules[{index}]"
        required = {
            "id",
            "logical_name",
            "version",
            "postgresql_major",
            "sdk_contract",
            "sdk_abi_version",
            "build",
            "capabilities",
            "lifecycle",
            "state_policy",
            "resources",
            "dependencies",
            "symbols",
        }
        if not isinstance(raw_module, dict) or set(raw_module) != required:
            raise StaticModuleBundleError(f"{label} schema is invalid")
        module_id = raw_module.get("id")
        logical_name = raw_module.get("logical_name")
        version = raw_module.get("version")
        if (
            not isinstance(module_id, str)
            or not MODULE_ID.fullmatch(module_id)
            or module_id in module_ids
        ):
            raise StaticModuleBundleError(f"{label}.id is invalid or duplicated")
        if (
            not isinstance(logical_name, str)
            or not LOGICAL_NAME.fullmatch(logical_name)
            or logical_name in logical_names
        ):
            raise StaticModuleBundleError(
                f"{label}.logical_name is invalid or duplicated"
            )
        if not isinstance(version, str) or not version:
            raise StaticModuleBundleError(f"{label}.version is invalid")
        c_string_literal(version, f"{label}.version")
        if raw_module.get("postgresql_major") != postgresql_major:
            raise StaticModuleBundleError(f"{label}.postgresql_major is incompatible")
        sdk_contract = raw_module.get("sdk_contract")
        sdk_abi_version = raw_module.get("sdk_abi_version")
        if not isinstance(sdk_contract, bool) or not isinstance(sdk_abi_version, int):
            raise StaticModuleBundleError(f"{label} SDK identity is invalid")
        if (sdk_contract and sdk_abi_version != 1) or (
            not sdk_contract and sdk_abi_version != 0
        ):
            raise StaticModuleBundleError(f"{label}.sdk_abi_version is invalid")
        raw_capabilities = raw_module.get("capabilities")
        if not isinstance(raw_capabilities, dict) or set(raw_capabilities) != set(
            CAPABILITY_NAMES
        ) or not all(isinstance(value, bool) for value in raw_capabilities.values()):
            raise StaticModuleBundleError(f"{label}.capabilities schema is invalid")
        capabilities = [
            name for name in CAPABILITY_NAMES if raw_capabilities[name]
        ]
        required_safe = set(CAPABILITY_NAMES[:4])
        if sdk_contract and not required_safe.issubset(capabilities):
            raise StaticModuleBundleError(f"{label} lacks required safety capabilities")
        unsupported_sdk = {
            "background-worker",
            "filesystem-write",
            "host-library-dependency",
            "process-global-state",
        }
        if sdk_contract and unsupported_sdk.intersection(capabilities):
            raise StaticModuleBundleError(
                f"{label} declares an unsupported SDK capability"
            )
        raw_build = raw_module.get("build")
        if not isinstance(raw_build, dict) or raw_build.get("kind") not in {
            "postgres-link-output",
            "project-source",
        }:
            raise StaticModuleBundleError(f"{label}.build is invalid")
        build_kind = raw_build["kind"]
        expected_build_fields = (
            {"kind", "link_output_suffix"}
            if build_kind == "postgres-link-output"
            else {"kind", "source"}
        )
        if set(raw_build) != expected_build_fields:
            raise StaticModuleBundleError(f"{label}.build schema is invalid")
        build = dict(raw_build)
        build_field = "link_output_suffix" if build_kind == "postgres-link-output" else "source"
        build[build_field] = safe_relative_path(
            build[build_field], f"{label}.build.{build_field}"
        )
        raw_lifecycle = raw_module.get("lifecycle")
        allowed_lifecycle = set(LIFECYCLE_BITS)
        if (
            not isinstance(raw_lifecycle, list)
            or len(raw_lifecycle) != len(set(raw_lifecycle))
            or any(item not in allowed_lifecycle for item in raw_lifecycle)
        ):
            raise StaticModuleBundleError(f"{label}.lifecycle is invalid")
        required_lifecycle = {
            "library-initialize",
            "instance-request",
            "instance-startup",
            "instance-shutdown",
        }
        session_lifecycle = {"session-initialize", "session-destroy"}
        if sdk_contract and not required_lifecycle.issubset(raw_lifecycle):
            raise StaticModuleBundleError(f"{label}.lifecycle is incomplete")
        if len(session_lifecycle.intersection(raw_lifecycle)) == 1 or (
            "session-reset" in raw_lifecycle
            and not session_lifecycle.issubset(raw_lifecycle)
        ):
            raise StaticModuleBundleError(
                f"{label}.session lifecycle is incomplete"
            )
        state_policy = raw_module.get("state_policy")
        if sdk_contract:
            state_policy = safe_relative_path(
                state_policy, f"{label}.state_policy"
            )
        elif state_policy is not None:
            raise StaticModuleBundleError(f"{label}.state_policy must be null")
        raw_resources = raw_module.get("resources")
        if not isinstance(raw_resources, list):
            raise StaticModuleBundleError(f"{label}.resources is invalid")
        resources: list[dict[str, Any]] = []
        resource_paths: set[str] = set()
        for resource_index, raw_resource in enumerate(raw_resources):
            resource_label = f"{label}.resources[{resource_index}]"
            if not isinstance(raw_resource, dict) or set(raw_resource) != {
                "kind",
                "source",
                "logical_path",
                "required",
            }:
                raise StaticModuleBundleError(f"{resource_label} schema is invalid")
            if raw_resource.get("kind") not in {
                "control",
                "sql",
                "data",
                "dictionary",
            } or not isinstance(raw_resource.get("required"), bool):
                raise StaticModuleBundleError(f"{resource_label} identity is invalid")
            source = safe_relative_path(raw_resource.get("source"), f"{resource_label}.source")
            logical_path = safe_relative_path(
                raw_resource.get("logical_path"), f"{resource_label}.logical_path"
            )
            if logical_path in resource_paths:
                raise StaticModuleBundleError(f"{resource_label}.logical_path is duplicated")
            resource_paths.add(logical_path)
            resources.append(
                {
                    "kind": raw_resource["kind"],
                    "source": source,
                    "logical_path": logical_path,
                    "required": raw_resource["required"],
                }
            )
        if sdk_contract and resources and not raw_capabilities["filesystem-read"]:
            raise StaticModuleBundleError(
                f"{label} declares resources without filesystem-read"
            )
        dependencies = raw_module.get("dependencies")
        if (
            not isinstance(dependencies, list)
            or len(dependencies) != len(set(dependencies))
            or any(not isinstance(item, str) or not MODULE_ID.fullmatch(item) for item in dependencies)
        ):
            raise StaticModuleBundleError(f"{label}.dependencies is invalid")
        symbols = _validate_symbols(raw_module.get("symbols"), label, linker_names)
        module_ids.add(module_id)
        logical_names.add(logical_name)
        modules.append(
            {
                "id": module_id,
                "logical_name": logical_name,
                "version": version,
                "postgresql_major": postgresql_major,
                "sdk_contract": sdk_contract,
                "sdk_abi_version": sdk_abi_version,
                "capabilities": capabilities,
                "capability_bits": sum(CAPABILITY_BITS[name] for name in capabilities),
                "build": build,
                "link_output_suffix": build.get("link_output_suffix", ""),
                "state_policy": state_policy or "",
                "lifecycle": sorted(raw_lifecycle),
                "lifecycle_bits": sum(
                    LIFECYCLE_BITS[item] for item in raw_lifecycle
                ),
                "resources": sorted(resources, key=lambda item: item["logical_path"]),
                "dependencies": sorted(dependencies),
                "symbols": symbols,
            }
        )
    known_ids = {module["id"] for module in modules}
    for module in modules:
        unknown = set(module["dependencies"]) - known_ids
        if unknown or module["id"] in module["dependencies"]:
            raise StaticModuleBundleError(
                f"module {module['id']} has invalid dependencies"
            )
    return {
        "schema_version": 2,
        "kind": MANIFEST_KIND,
        "id": document["id"],
        "postgresql_major": postgresql_major,
        "capability_vocabulary": list(CAPABILITY_NAMES),
        "modules": sorted(modules, key=lambda item: item["logical_name"]),
    }


def validate_link_database(document: dict[str, Any]) -> dict[str, Any]:
    if set(document) != {
        "schema_version",
        "kind",
        "build_root",
        "identity",
        "commands",
    }:
        raise StaticModuleBundleError("link database schema is invalid")
    if (
        document.get("schema_version") != 1
        or document.get("kind") != LINK_DATABASE_KIND
        or not isinstance(document.get("build_root"), str)
        or not document["build_root"]
        or not isinstance(document.get("identity"), dict)
        or not isinstance(document.get("commands"), list)
    ):
        raise StaticModuleBundleError("link database identity is invalid")
    return document


def resolve_module_inputs(
    link_database: dict[str, Any],
    module: dict[str, Any],
    project_root: Path | None = None,
    module_objects: dict[str, Path] | None = None,
) -> list[Path]:
    if module["build"]["kind"] == "project-source":
        if project_root is None or module_objects is None:
            raise StaticModuleBundleError(
                f"module {module['logical_name']} requires a project object"
            )
        source = (project_root / module["build"]["source"]).resolve()
        try:
            source.relative_to(project_root.resolve())
        except ValueError as exc:
            raise StaticModuleBundleError(
                f"module source is outside the project root: {source}"
            ) from exc
        if not source.is_file():
            raise StaticModuleBundleError(f"module source is missing: {source}")
        object_path = module_objects.get(module["id"])
        if object_path is None or not object_path.is_file():
            raise StaticModuleBundleError(
                f"module {module['logical_name']} project object is missing"
            )
        return [object_path.resolve()]
    commands = link_database.get("commands")
    build_root_value = link_database.get("build_root")
    if not isinstance(commands, list) or not isinstance(build_root_value, str):
        raise StaticModuleBundleError("link database schema is invalid")
    build_root = Path(build_root_value).resolve()
    suffix = Path(module["link_output_suffix"])
    matches: list[dict[str, Any]] = []
    for command in commands:
        if not isinstance(command, dict) or not isinstance(command.get("output"), str):
            continue
        output = Path(command["output"]).resolve()
        try:
            relative = output.relative_to(build_root)
        except ValueError:
            continue
        if relative == suffix:
            matches.append(command)
    if len(matches) != 1:
        raise StaticModuleBundleError(
            f"module {module['logical_name']} matched {len(matches)} link commands"
        )
    ordered_inputs = matches[0].get("ordered_inputs")
    if not isinstance(ordered_inputs, list) or not ordered_inputs:
        raise StaticModuleBundleError(
            f"module {module['logical_name']} has no captured link inputs"
        )
    result: list[Path] = []
    for item in ordered_inputs:
        if (
            not isinstance(item, dict)
            or item.get("kind") != "object"
            or not isinstance(item.get("path"), str)
        ):
            raise StaticModuleBundleError(
                f"module {module['logical_name']} has a non-object link input"
            )
        path = Path(item["path"]).resolve()
        try:
            path.relative_to(build_root)
        except ValueError as exc:
            raise StaticModuleBundleError(
                f"module input is outside the captured build root: {path}"
            ) from exc
        if not path.is_file():
            raise StaticModuleBundleError(f"module input is missing: {path}")
        result.append(path)
    if len(result) != len(set(result)):
        raise StaticModuleBundleError(
            f"module {module['logical_name']} contains duplicate link inputs"
        )
    return result


def render_facts(manifest: dict[str, Any]) -> str:
    lines = ["/* Generated embedded static-module facts.  Do not edit. */", ""]
    for module in manifest["modules"]:
        for symbol in module["symbols"]:
            lines.append(f"extern void {symbol['linker_name']}(void);")
    lines.append("")
    for module in manifest["modules"]:
        lines.extend(
            (
                f"static const PostgammaProductModuleSymbol "
                f"PostgammaProductModuleSymbols_{module['id']}[] =",
                "{",
            )
        )
        for symbol in module["symbols"]:
            logical_name = c_string_literal(
                symbol["logical_name"], "module symbol logical_name"
            )
            lines.append(
                f"\t{{{logical_name}, "
                f'(void *) {symbol["linker_name"]}}},'
            )
        lines.extend(("};", ""))
        if module["resources"]:
            lines.extend(
                (
                    f"static const PostgammaProductModuleResource "
                    f"PostgammaProductModuleResources_{module['id']}[] =",
                    "{",
                )
            )
            for resource in module["resources"]:
                required = "true" if resource["required"] else "false"
                logical_path = c_string_literal(
                    resource["logical_path"], "module resource logical_path"
                )
                lines.append(
                    f"\t{{{logical_path}, {required}}},"
                )
            lines.extend(("};", ""))
    lines.extend(("static const PostgammaProductModule PostgammaProductModules[] =", "{"))
    for module in manifest["modules"]:
        array = f"PostgammaProductModuleSymbols_{module['id']}"
        if module["resources"]:
            resources = f"PostgammaProductModuleResources_{module['id']}"
            resource_count = f"sizeof({resources}) / sizeof({resources}[0])"
        else:
            resources = "NULL"
            resource_count = "0"
        sdk_contract = "true" if module["sdk_contract"] else "false"
        module_id = c_string_literal(module["id"], "module id")
        logical_name = c_string_literal(
            module["logical_name"], "module logical_name"
        )
        version = c_string_literal(module["version"], "module version")
        lines.append(
            f"\t{{{module_id}, {logical_name}, {version}, "
            f'UINT32_C({module["postgresql_major"]}), '
            f'UINT32_C({module["sdk_abi_version"]}), '
            f'UINT64_C({module["capability_bits"]}), '
            f'UINT32_C({module["lifecycle_bits"]}), {sdk_contract}, '
            f"{array}, sizeof({array}) / sizeof({array}[0]), {resources}, "
            f"{resource_count}}},"
        )
    lines.extend(
        (
            "};",
            "",
            "static const size_t PostgammaProductModuleCount =",
            "\tsizeof(PostgammaProductModules) / sizeof(PostgammaProductModules[0]);",
            "",
        )
    )
    return "\n".join(lines)


def atomic_write_text(path: Path, content: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(
        mode="w",
        encoding="utf-8",
        dir=path.parent,
        prefix=f".{path.name}.",
        delete=False,
    ) as handle:
        handle.write(content)
        temporary = Path(handle.name)
    try:
        os.replace(temporary, path)
    finally:
        temporary.unlink(missing_ok=True)


def bundle(
    manifest_path: Path,
    link_database_path: Path,
    compiler: str,
    objcopy: str,
    nm: str,
    output: Path,
    facts: Path,
    receipt: Path,
    project_root: Path | None = None,
    module_objects: dict[str, Path] | None = None,
) -> dict[str, Any]:
    manifest = validate_manifest(read_json(manifest_path))
    link_database = validate_link_database(read_json(link_database_path))
    module_inputs = {
        module["id"]: resolve_module_inputs(
            link_database, module, project_root, module_objects
        )
        for module in manifest["modules"]
    }
    output.parent.mkdir(parents=True, exist_ok=True)
    commands: list[list[str]] = []
    module_reports: list[dict[str, Any]] = []
    with tempfile.TemporaryDirectory(
        prefix=f".{output.name}.", dir=output.parent
    ) as directory:
        temporary_root = Path(directory)
        localized_objects: list[Path] = []
        for module in manifest["modules"]:
            module_id = module["id"]
            raw_object = temporary_root / f"{module_id}-raw.o"
            localized_object = temporary_root / f"{module_id}.o"
            localize_file = temporary_root / f"{module_id}-localize.txt"
            link_command = [
                compiler,
                "-r",
                "-o",
                str(raw_object),
                *(str(path) for path in module_inputs[module_id]),
            ]
            run(link_command, f"{module_id} partial link")
            commands.append(link_command)
            definitions = global_definitions(nm, raw_object)
            symbol_map = {
                symbol["logical_name"]: symbol["linker_name"]
                for symbol in module["symbols"]
            }
            missing = sorted(set(symbol_map) - set(definitions))
            if missing:
                raise StaticModuleBundleError(
                    f"module {module_id} is missing registered symbol(s): "
                    + ", ".join(missing)
                )
            mapped_definitions = [symbol_map.get(name, name) for name in definitions]
            kept = set(symbol_map.values())
            localized = sorted(set(mapped_definitions) - kept)
            localize_file.write_text(
                "\n".join(localized) + ("\n" if localized else ""),
                encoding="utf-8",
            )
            objcopy_command = [objcopy]
            for logical_name, linker_name in sorted(symbol_map.items()):
                objcopy_command.extend(
                    ["--redefine-sym", f"{logical_name}={linker_name}"]
                )
            if localized:
                objcopy_command.append(f"--localize-symbols={localize_file}")
            objcopy_command.extend([str(raw_object), str(localized_object)])
            run(objcopy_command, f"{module_id} symbol localization")
            commands.append(objcopy_command)
            final_definitions = global_definitions(nm, localized_object)
            if final_definitions != sorted(kept):
                raise StaticModuleBundleError(
                    f"module {module_id} retained unexpected global symbols"
                )
            localized_objects.append(localized_object)
            module_reports.append(
                {
                    "id": module_id,
                    "logical_name": module["logical_name"],
                    "version": module["version"],
                    "postgresql_major": module["postgresql_major"],
                    "sdk_contract": module["sdk_contract"],
                    "sdk_abi_version": module["sdk_abi_version"],
                    "capabilities": module["capabilities"],
                    "capability_bits": module["capability_bits"],
                    "lifecycle": module["lifecycle"],
                    "lifecycle_bits": module["lifecycle_bits"],
                    "build": module["build"],
                    "state_policy": module["state_policy"],
                    "resources": module["resources"],
                    "dependencies": module["dependencies"],
                    "object_inputs": [str(path) for path in module_inputs[module_id]],
                    "registered_symbols": sorted(kept),
                    "localized_symbol_count": len(localized),
                }
            )
        combined = temporary_root / output.name
        combine_command = [
            compiler,
            "-r",
            "-o",
            str(combined),
            *(str(path) for path in localized_objects),
        ]
        run(combine_command, "static module bundle link")
        commands.append(combine_command)
        expected = sorted(
            symbol["linker_name"]
            for module in manifest["modules"]
            for symbol in module["symbols"]
        )
        if global_definitions(nm, combined) != expected:
            raise StaticModuleBundleError(
                "combined static module object retained unexpected global symbols"
            )
        os.replace(combined, output)

    atomic_write_text(facts, render_facts(manifest))
    input_paths = sorted(
        {path for paths in module_inputs.values() for path in paths}, key=str
    )
    document: dict[str, Any] = {
        "schema_version": 1,
        "kind": RECEIPT_KIND,
        "manifest_id": manifest["id"],
        "manifest_sha256": sha256(manifest_path),
        "link_database_sha256": sha256(link_database_path),
        "postgresql_major": manifest.get("postgresql_major", 0),
        "capability_vocabulary": manifest.get("capability_vocabulary", []),
        "modules": module_reports,
        "commands": commands,
        "inputs": {str(path): sha256(path) for path in input_paths},
        "output": str(output),
        "output_sha256": sha256(output),
        "facts": str(facts),
        "facts_sha256": sha256(facts),
    }
    atomic_write_text(receipt, json.dumps(document, indent=2, sort_keys=True) + "\n")
    return document


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--manifest", required=True, type=Path)
    parser.add_argument("--link-database", required=True, type=Path)
    parser.add_argument("--compiler", default="cc")
    parser.add_argument("--objcopy", default="objcopy")
    parser.add_argument("--nm", default="nm")
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--facts", required=True, type=Path)
    parser.add_argument("--receipt", required=True, type=Path)
    parser.add_argument("--project-root", type=Path)
    parser.add_argument(
        "--module-object",
        action="append",
        default=[],
        metavar="MODULE_ID=PATH",
    )
    args = parser.parse_args()
    module_objects: dict[str, Path] = {}
    for mapping in args.module_object:
        module_id, separator, path = mapping.partition("=")
        if (
            not separator
            or not MODULE_ID.fullmatch(module_id)
            or not path
            or module_id in module_objects
        ):
            parser.error(f"invalid --module-object mapping: {mapping}")
        module_objects[module_id] = Path(path).resolve()
    try:
        document = bundle(
            args.manifest.resolve(),
            args.link_database.resolve(),
            args.compiler,
            args.objcopy,
            args.nm,
            args.output.resolve(),
            args.facts.resolve(),
            args.receipt.resolve(),
            args.project_root.resolve() if args.project_root else None,
            module_objects,
        )
    except (OSError, StaticModuleBundleError) as exc:
        parser.error(str(exc))
    print(
        "embedded static modules: "
        f"{len(document['modules'])} module(s), "
        f"{sum(len(module['registered_symbols']) for module in document['modules'])} "
        "registered symbol(s)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
