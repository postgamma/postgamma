#!/usr/bin/env python3
"""Load the PostgreSQL-major-specific embedded integration adapter."""

from __future__ import annotations

import hashlib
import json
import re
from collections import Counter
from pathlib import Path
from typing import Any


ADAPTER_KIND = "postgamma.postgresql-embedded-adapter"
PROCESS_ASSUMPTION_CLASSIFICATIONS = frozenset(
    {
        "removed_for_embedding",
        "provider_routed_for_embedding",
        "test_only_compatibility",
        "required_for_embedded_kernel",
        "required_for_async_execution",
        "required_for_c_api",
        "required_for_logical_management",
        "forbidden",
    }
)
PROCESS_ASSUMPTION_CATEGORIES = frozenset(
    {
        "crash_restart",
        "cwd",
        "dynamic_loader",
        "exit",
        "extension_shmem",
        "frontend_tool",
        "libpq_network",
        "pidfile",
        "process_creation",
        "server_network",
        "signal_emission",
        "thread_stack",
    }
)
GUC_NAME_PATTERN = re.compile(r"^[a-z][a-z0-9_]*$")
COMPILE_CONDITION_PATTERN = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")


class EmbeddedAdapterError(ValueError):
    """The embedded adapter is malformed or outside the PG19 contract."""


def _unique_object(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    counts = Counter(key for key, _value in pairs)
    duplicates = sorted(key for key, count in counts.items() if count > 1)
    if duplicates:
        raise EmbeddedAdapterError(
            "duplicate JSON key(s): " + ", ".join(duplicates)
        )
    return dict(pairs)


def load_document(path: Path) -> dict[str, Any]:
    try:
        document = json.loads(
            path.read_text(encoding="utf-8"), object_pairs_hook=_unique_object
        )
    except (OSError, json.JSONDecodeError, EmbeddedAdapterError) as exc:
        raise EmbeddedAdapterError(f"cannot read {path}: {exc}") from exc
    if not isinstance(document, dict):
        raise EmbeddedAdapterError(f"{path}: top-level value must be an object")
    return document


def validate_adapter(document: dict[str, Any]) -> dict[str, Any]:
    allowed = {
        "schema_version",
        "kind",
        "id",
        "product_postgresql_major",
        "kernel_link_target",
        "module_provider_seam",
        "bootstrap_seam",
        "frontend_tool_hooks",
        "libpq_memory_hooks",
        "process_assumptions",
        "kernel_exclusions",
        "catalog_registration_roots",
        "guc_policy",
    }
    unknown = sorted(set(document) - allowed)
    if unknown:
        raise EmbeddedAdapterError(
            "embedded adapter has unknown field(s): " + ", ".join(unknown)
        )
    if document.get("schema_version") != 1 or document.get("kind") != ADAPTER_KIND:
        raise EmbeddedAdapterError(
            f"embedded adapter must use schema_version 1 and kind {ADAPTER_KIND}"
        )
    for field in ("id", "kernel_link_target"):
        if not isinstance(document.get(field), str) or not document[field]:
            raise EmbeddedAdapterError(
                f"embedded adapter {field} must be a non-empty string"
            )
    if document.get("product_postgresql_major") != 19:
        raise EmbeddedAdapterError(
            "embedded adapter product PostgreSQL major must be 19"
        )

    def source_seam(field: str) -> dict[str, Any]:
        seam = document.get(field)
        if not isinstance(seam, dict) or set(seam) != {
            "source_file",
            "enclosing_function",
            "expected_definitions",
            "required_callees",
        }:
            raise EmbeddedAdapterError(
                f"embedded adapter {field} has an invalid schema"
            )
        for name in ("source_file", "enclosing_function"):
            if not isinstance(seam[name], str) or not seam[name]:
                raise EmbeddedAdapterError(
                    f"embedded adapter {field} {name} is invalid"
                )
        if seam["source_file"].startswith("/") or ".." in Path(
            seam["source_file"]
        ).parts:
            raise EmbeddedAdapterError(
                f"embedded adapter {field} source_file is unsafe"
            )
        if (
            not isinstance(seam["expected_definitions"], int)
            or isinstance(seam["expected_definitions"], bool)
            or seam["expected_definitions"] != 1
        ):
            raise EmbeddedAdapterError(
                f"embedded adapter {field} must expect one definition"
            )
        callees = seam["required_callees"]
        if not isinstance(callees, list) or not callees:
            raise EmbeddedAdapterError(
                f"embedded adapter {field} required_callees is invalid"
            )
        callee_names: set[str] = set()
        for index, callee in enumerate(callees):
            if not isinstance(callee, dict) or set(callee) != {
                "name",
                "expected_matches",
            }:
                raise EmbeddedAdapterError(
                    f"embedded adapter {field} required_callees[{index}] "
                    "has an invalid schema"
                )
            name = callee["name"]
            expected = callee["expected_matches"]
            if (
                not isinstance(name, str)
                or not name
                or name in callee_names
                or not isinstance(expected, int)
                or isinstance(expected, bool)
                or expected < 1
            ):
                raise EmbeddedAdapterError(
                    f"embedded adapter {field} required_callees[{index}] is invalid"
                )
            callee_names.add(name)
        return {
            **seam,
            "required_callees": sorted(callees, key=lambda item: item["name"]),
        }

    module_provider_seam = source_seam("module_provider_seam")
    bootstrap_seam = source_seam("bootstrap_seam")

    frontend_tool_hooks = document.get("frontend_tool_hooks")
    if not isinstance(frontend_tool_hooks, list) or not frontend_tool_hooks:
        raise EmbeddedAdapterError(
            "embedded adapter frontend_tool_hooks must be a non-empty array"
        )
    normalized_frontend_tool_hooks: list[dict[str, Any]] = []
    frontend_tool_hook_ids: set[str] = set()
    for index, hook in enumerate(frontend_tool_hooks):
        label = f"embedded adapter frontend_tool_hooks[{index}]"
        fields = {
            "id",
            "source_file",
            "enclosing_function",
            "expected_definitions",
            "anchor",
            "replacement",
            "expected_matches",
        }
        if not isinstance(hook, dict) or set(hook) != fields:
            raise EmbeddedAdapterError(f"{label} has an invalid schema")
        for name in (
            "id",
            "source_file",
            "enclosing_function",
            "anchor",
            "replacement",
        ):
            if not isinstance(hook[name], str) or not hook[name]:
                raise EmbeddedAdapterError(f"{label}.{name} is invalid")
        if hook["id"] in frontend_tool_hook_ids:
            raise EmbeddedAdapterError(f"{label}.id is duplicated")
        frontend_tool_hook_ids.add(hook["id"])
        source_path = Path(hook["source_file"])
        if source_path.is_absolute() or ".." in source_path.parts:
            raise EmbeddedAdapterError(f"{label}.source_file is unsafe")
        for name in ("expected_definitions", "expected_matches"):
            if (
                not isinstance(hook[name], int)
                or isinstance(hook[name], bool)
                or hook[name] < 1
            ):
                raise EmbeddedAdapterError(f"{label}.{name} must be positive")
        if "POSTGAMMA_INITDB_PHASE_SELECT" not in hook["replacement"]:
            raise EmbeddedAdapterError(
                f"{label}.replacement must select a typed initdb phase"
            )
        normalized_frontend_tool_hooks.append(hook)

    hooks = document.get("libpq_memory_hooks")
    if not isinstance(hooks, list) or not hooks:
        raise EmbeddedAdapterError(
            "embedded adapter libpq_memory_hooks must be a non-empty array"
        )
    normalized_hooks: list[dict[str, Any]] = []
    hook_ids: set[str] = set()
    for index, hook in enumerate(hooks):
        label = f"embedded adapter libpq_memory_hooks[{index}]"
        if not isinstance(hook, dict):
            raise EmbeddedAdapterError(f"{label} must be an object")
        common = {
            "id",
            "source_file",
            "enclosing_function",
            "expected_definitions",
            "kind",
        }
        kind = hook.get("kind")
        expected = common | (
            {"macro", "arguments"}
            if kind == "entry_macro"
            else {"anchor", "replacement"}
            if kind == "replace_once"
            else set()
        )
        if kind not in {"entry_macro", "replace_once"} or set(hook) != expected:
            raise EmbeddedAdapterError(f"{label} has an invalid schema")
        for name in ("id", "source_file", "enclosing_function"):
            if not isinstance(hook[name], str) or not hook[name]:
                raise EmbeddedAdapterError(f"{label}.{name} is invalid")
        if hook["id"] in hook_ids:
            raise EmbeddedAdapterError(f"{label}.id is duplicated")
        hook_ids.add(hook["id"])
        source_path = Path(hook["source_file"])
        if source_path.is_absolute() or ".." in source_path.parts:
            raise EmbeddedAdapterError(f"{label}.source_file is unsafe")
        if hook["expected_definitions"] != 1:
            raise EmbeddedAdapterError(
                f"{label}.expected_definitions must be exactly one"
            )
        if kind == "entry_macro":
            if not isinstance(hook["macro"], str) or not hook["macro"].startswith(
                "POSTGAMMA_LIBPQ_"
            ):
                raise EmbeddedAdapterError(f"{label}.macro is invalid")
            arguments = hook["arguments"]
            if not isinstance(arguments, list) or not all(
                isinstance(argument, str) and argument for argument in arguments
            ):
                raise EmbeddedAdapterError(f"{label}.arguments is invalid")
        else:
            if not all(
                isinstance(hook[name], str) and hook[name]
                for name in ("anchor", "replacement")
            ):
                raise EmbeddedAdapterError(
                    f"{label} anchor/replacement is invalid"
                )
            if "POSTGAMMA_LIBPQ_" not in hook["replacement"]:
                raise EmbeddedAdapterError(
                    f"{label}.replacement must use a libpq provider macro"
                )
        normalized_hooks.append(hook)

    assumptions = document.get("process_assumptions")
    if not isinstance(assumptions, list) or not assumptions:
        raise EmbeddedAdapterError(
            "embedded adapter process_assumptions must be a non-empty array"
        )
    normalized_assumptions: list[dict[str, Any]] = []
    assumption_ids: set[str] = set()
    for index, assumption in enumerate(assumptions):
        label = f"embedded adapter process_assumptions[{index}]"
        fields = {
            "id",
            "category",
            "source_root",
            "source_file",
            "scope",
            "probe_kind",
            "probe",
            "expected_matches",
            "classification",
            "rationale",
        }
        if not isinstance(assumption, dict) or set(assumption) != fields:
            raise EmbeddedAdapterError(f"{label} has an invalid schema")
        for name in ("id", "source_file", "scope", "probe", "rationale"):
            if not isinstance(assumption[name], str) or not assumption[name]:
                raise EmbeddedAdapterError(f"{label}.{name} is invalid")
        if assumption["id"] in assumption_ids:
            raise EmbeddedAdapterError(f"{label}.id is duplicated")
        assumption_ids.add(assumption["id"])
        if assumption["category"] not in PROCESS_ASSUMPTION_CATEGORIES:
            raise EmbeddedAdapterError(f"{label}.category is invalid")
        if assumption["source_root"] not in {"postgresql", "project"}:
            raise EmbeddedAdapterError(f"{label}.source_root is invalid")
        source_path = Path(assumption["source_file"])
        if source_path.is_absolute() or ".." in source_path.parts:
            raise EmbeddedAdapterError(f"{label}.source_file is unsafe")
        if assumption["probe_kind"] not in {"call", "text"}:
            raise EmbeddedAdapterError(f"{label}.probe_kind is invalid")
        matches = assumption["expected_matches"]
        if (
            not isinstance(matches, int)
            or isinstance(matches, bool)
            or matches < 0
        ):
            raise EmbeddedAdapterError(
                f"{label}.expected_matches must be non-negative"
            )
        if assumption["classification"] not in PROCESS_ASSUMPTION_CLASSIFICATIONS:
            raise EmbeddedAdapterError(f"{label}.classification is invalid")
        normalized_assumptions.append(assumption)

    guc_policy = document.get("guc_policy")
    if not isinstance(guc_policy, dict) or set(guc_policy) != {
        "defaults",
        "safety",
    }:
        raise EmbeddedAdapterError(
            "embedded adapter guc_policy has an invalid schema"
        )

    def guc_settings(field: str) -> list[dict[str, str]]:
        values = guc_policy[field]
        if not isinstance(values, list) or not values:
            raise EmbeddedAdapterError(
                f"embedded adapter guc_policy.{field} must be a non-empty array"
            )
        result: list[dict[str, str]] = []
        names: set[str] = set()
        for index, value in enumerate(values):
            label = f"embedded adapter guc_policy.{field}[{index}]"
            if not isinstance(value, dict) or set(value) != {
                "name",
                "value",
                "compile_condition",
            }:
                raise EmbeddedAdapterError(f"{label} has an invalid schema")
            name = value["name"]
            setting_value = value["value"]
            condition = value["compile_condition"]
            if (
                not isinstance(name, str)
                or GUC_NAME_PATTERN.fullmatch(name) is None
                or name in names
            ):
                raise EmbeddedAdapterError(f"{label}.name is invalid or duplicated")
            if not isinstance(setting_value, str):
                raise EmbeddedAdapterError(f"{label}.value must be a string")
            if not isinstance(condition, str) or (
                condition
                and COMPILE_CONDITION_PATTERN.fullmatch(condition) is None
            ):
                raise EmbeddedAdapterError(
                    f"{label}.compile_condition is invalid"
                )
            names.add(name)
            result.append(value)
        return result

    normalized_guc_policy = {
        "defaults": guc_settings("defaults"),
        "safety": guc_settings("safety"),
    }

    def rules(field: str) -> list[dict[str, Any]]:
        values = document.get(field)
        if not isinstance(values, list) or not values:
            raise EmbeddedAdapterError(
                f"embedded adapter {field} must be a non-empty array"
            )
        result: list[dict[str, Any]] = []
        identifiers: set[str] = set()
        for index, value in enumerate(values):
            if not isinstance(value, dict) or set(value) != {
                "id",
                "path_suffix",
                "expected_matches",
                "reason",
            }:
                raise EmbeddedAdapterError(
                    f"embedded adapter {field}[{index}] has an invalid schema"
                )
            if not all(
                isinstance(value[name], str) and value[name]
                for name in ("id", "path_suffix", "reason")
            ):
                raise EmbeddedAdapterError(
                    f"embedded adapter {field}[{index}] has an empty string fact"
                )
            if value["path_suffix"].startswith("/") or ".." in Path(
                value["path_suffix"]
            ).parts:
                raise EmbeddedAdapterError(
                    f"embedded adapter {field}[{index}] path suffix is unsafe"
                )
            if not isinstance(value["expected_matches"], int) or value[
                "expected_matches"
            ] < 1:
                raise EmbeddedAdapterError(
                    f"embedded adapter {field}[{index}] expected_matches must be positive"
                )
            if value["id"] in identifiers:
                raise EmbeddedAdapterError(
                    f"embedded adapter {field} contains duplicate id {value['id']}"
                )
            identifiers.add(value["id"])
            result.append(value)
        return result

    return {
        **document,
        "module_provider_seam": module_provider_seam,
        "bootstrap_seam": bootstrap_seam,
        "frontend_tool_hooks": sorted(
            normalized_frontend_tool_hooks, key=lambda item: item["id"]
        ),
        "libpq_memory_hooks": sorted(
            normalized_hooks, key=lambda item: item["id"]
        ),
        "process_assumptions": sorted(
            normalized_assumptions, key=lambda item: item["id"]
        ),
        "guc_policy": normalized_guc_policy,
        "kernel_exclusions": rules("kernel_exclusions"),
        "catalog_registration_roots": rules("catalog_registration_roots"),
    }


def load_adapter(path: Path) -> dict[str, Any]:
    return validate_adapter(load_document(path))


def adapter_sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()
