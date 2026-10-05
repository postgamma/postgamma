#!/usr/bin/env python3
"""Compile a reviewed extension-state policy into source replacements."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
from collections import Counter, defaultdict
from pathlib import Path
from typing import Any

from generate_backend_state_plan import safe_source
from state_policy import (
    align_exact_candidates,
    catalog_candidates,
    load_json,
    policy_decisions,
    reject_unknown,
)


POLICY_KIND = "postgamma.extension-state-ownership"
PLAN_KIND = "postgamma.extension-state-plan"
REPORT_KIND = "postgamma.extension-state-alignment"
BACKEND_POLICY_KIND = "postgamma.backend-state-ownership"
BACKEND_RUNTIME_KIND = "postgamma.backend-state-runtime"
GUC_MANIFEST_KIND = "postgamma.ast-transform"
OWNERS = ("immutable", "role")
IDENTIFIER = re.compile(r"[A-Za-z_][A-Za-z0-9_]*\Z")
EXTENSION_ID = re.compile(r"[a-z][a-z0-9_]*\Z")
MAX_RUNTIME_IDENTIFIER = 255


class ExtensionStatePlanError(ValueError):
    """The extension catalog, policy, or source tree is inconsistent."""


def parse_policy(document: dict[str, Any]) -> dict[str, Any]:
    reject_unknown(
        document,
        (
            "schema_version",
            "kind",
            "extension_id",
            "accessor",
            "upstream",
            "decisions",
        ),
        "extension state policy",
        ExtensionStatePlanError,
    )
    extension_id = document.get("extension_id")
    accessor = document.get("accessor")
    upstream = document.get("upstream")
    if document.get("schema_version") != 1 or document.get("kind") != POLICY_KIND:
        raise ExtensionStatePlanError(
            f"policy: expected schema_version 1 and kind {POLICY_KIND}"
        )
    if not isinstance(extension_id, str) or EXTENSION_ID.fullmatch(extension_id) is None:
        raise ExtensionStatePlanError("policy: extension_id is invalid")
    if not isinstance(accessor, str) or IDENTIFIER.fullmatch(accessor) is None:
        raise ExtensionStatePlanError("policy: accessor is invalid")
    if not isinstance(upstream, dict) or set(upstream) != {
        "repository",
        "commit",
        "version",
    }:
        raise ExtensionStatePlanError("policy: upstream identity is invalid")
    for field in ("repository", "commit", "version"):
        if not isinstance(upstream.get(field), str) or not upstream[field]:
            raise ExtensionStatePlanError(f"policy: upstream.{field} is invalid")
    decisions = policy_decisions(
        document,
        policy_kind=POLICY_KIND,
        owners=OWNERS,
        error_type=ExtensionStatePlanError,
        allowed_top_fields=(
            "schema_version",
            "kind",
            "extension_id",
            "accessor",
            "upstream",
            "decisions",
        ),
    )
    return {
        "extension_id": extension_id,
        "accessor": accessor,
        "upstream": dict(upstream),
        "decisions": decisions,
    }


def compile_plan(
    catalog: dict[str, Any],
    policy_document: dict[str, Any],
    source_root: Path,
    backend_policy: dict[str, Any],
    backend_runtime: dict[str, Any],
    guc_manifest: dict[str, Any],
) -> tuple[dict[str, Any], dict[str, Any]]:
    candidates = catalog_candidates(catalog, ExtensionStatePlanError)
    policy = parse_policy(policy_document)
    decisions = policy["decisions"]
    dormant = align_exact_candidates(candidates, decisions, ExtensionStatePlanError)
    role_ids = {
        identifier
        for identifier in candidates
        if decisions[identifier]["owner"] == "role"
    }
    for identifier in sorted(role_ids):
        candidate = candidates[identifier]
        if not candidate.get("complete_type") or not candidate.get(
            "trivially_copyable"
        ):
            raise ExtensionStatePlanError(
                f"{identifier}: role state requires a complete, trivially-copyable type"
            )
        if not isinstance(candidate.get("size"), int) or candidate["size"] <= 0:
            raise ExtensionStatePlanError(f"{identifier}: invalid state size")
        if (
            not isinstance(candidate.get("alignment"), int)
            or candidate["alignment"] <= 0
            or candidate["alignment"] & (candidate["alignment"] - 1)
        ):
            raise ExtensionStatePlanError(f"{identifier}: invalid state alignment")

    file_replacements: dict[
        str, dict[tuple[int, int], dict[str, Any]]
    ] = defaultdict(dict)
    uses_by_kind: Counter[str] = Counter()
    transformed_ids: set[str] = set()
    for index, use in enumerate(catalog.get("uses", [])):
        if not isinstance(use, dict):
            raise ExtensionStatePlanError(f"catalog: uses[{index}] must be an object")
        identifier = use.get("id")
        if identifier not in role_ids:
            continue
        if use.get("source_kind", "source") != "source":
            raise ExtensionStatePlanError(
                f"{identifier}: extension state may only originate in the source tree"
            )
        if use.get("in_static_initializer"):
            raise ExtensionStatePlanError(
                f"{identifier}: address is embedded in a static initializer"
            )
        path = use.get("path")
        offset = use.get("offset")
        length = use.get("length")
        if not isinstance(path, str) or not isinstance(offset, int) or not isinstance(
            length, int
        ):
            raise ExtensionStatePlanError(
                f"catalog: uses[{index}] has an invalid source location"
            )
        candidate = candidates[identifier]
        name = candidate["name"]
        runtime_identifier = f"{policy['extension_id']}:{identifier}"
        if len(runtime_identifier.encode("utf-8")) > MAX_RUNTIME_IDENTIFIER:
            raise ExtensionStatePlanError(
                f"{identifier}: generated runtime identifier is too long"
            )
        replacement_text = (
            "POSTGAMMA_EXTENSION_ROLE_STATE_VALUE("
            f"{policy['accessor']}, "
            f"{json.dumps(runtime_identifier, ensure_ascii=True)}, {name})"
        )
        recorded_use_kind = (
            "macro_body"
            if use.get("macro_kind") == "body"
            else use.get("use_kind", "read")
        )
        replacement = {
            "offset": offset,
            "length": length,
            "original": name,
            "replacement": replacement_text,
            "kind": "extension_role_state",
            "rule_id": identifier,
            "use_kind": recorded_use_kind,
            "line": use.get("line", 0),
            "column": use.get("column", 0),
        }
        key = (offset, length)
        existing = file_replacements[path].get(key)
        if existing is not None:
            if any(
                existing[field] != replacement[field]
                for field in ("original", "replacement", "rule_id")
            ):
                raise ExtensionStatePlanError(
                    f"inconsistent semantic binding at {path}:{offset}+{length}"
                )
            if existing["use_kind"] != replacement["use_kind"]:
                existing["use_kind"] = "mixed"
            continue
        file_replacements[path][key] = replacement
        uses_by_kind[recorded_use_kind] += 1
        transformed_ids.add(identifier)

    if (
        backend_policy.get("schema_version") != 1
        or backend_policy.get("kind") != BACKEND_POLICY_KIND
        or not isinstance(backend_policy.get("decisions"), list)
    ):
        raise ExtensionStatePlanError("backend-state policy identity is invalid")
    core_decisions: dict[str, dict[str, Any]] = {}
    for index, decision in enumerate(backend_policy["decisions"]):
        if not isinstance(decision, dict):
            raise ExtensionStatePlanError(
                f"backend-state decisions[{index}] must be an object"
            )
        identifier = decision.get("id")
        owner = decision.get("owner")
        if (
            not isinstance(identifier, str)
            or not identifier.startswith("external:")
            or owner not in ("immutable", "instance", "managed_guc", "role", "session")
        ):
            continue
        if identifier in core_decisions:
            raise ExtensionStatePlanError(
                f"duplicate backend-state decision {identifier}"
            )
        core_decisions[identifier] = decision
    if (
        backend_runtime.get("schema_version") != 1
        or backend_runtime.get("kind") != BACKEND_RUNTIME_KIND
        or not isinstance(backend_runtime.get("slots"), list)
    ):
        raise ExtensionStatePlanError("backend-state runtime identity is invalid")
    runtime_by_id: dict[str, dict[str, Any]] = {}
    for index, slot in enumerate(backend_runtime["slots"]):
        if not isinstance(slot, dict) or not isinstance(slot.get("id"), str):
            raise ExtensionStatePlanError(
                f"backend-state runtime slots[{index}] is invalid"
            )
        if slot["id"] in runtime_by_id:
            raise ExtensionStatePlanError(
                f"duplicate backend-state runtime slot {slot['id']}"
            )
        runtime_by_id[slot["id"]] = slot
    if (
        guc_manifest.get("schema_version") != 1
        or guc_manifest.get("kind") != GUC_MANIFEST_KIND
        or not isinstance(guc_manifest.get("symbols"), list)
    ):
        raise ExtensionStatePlanError("GUC transform manifest identity is invalid")
    guc_by_name: dict[str, dict[str, Any]] = {}
    for index, symbol in enumerate(guc_manifest["symbols"]):
        if (
            not isinstance(symbol, dict)
            or not isinstance(symbol.get("name"), str)
            or not isinstance(symbol.get("replacement"), str)
        ):
            raise ExtensionStatePlanError(
                f"GUC transform symbols[{index}] is invalid"
            )
        if symbol["name"] in guc_by_name:
            raise ExtensionStatePlanError(
                f"duplicate GUC transform symbol {symbol['name']}"
            )
        guc_by_name[symbol["name"]] = symbol

    core_reference_ids: set[str] = set()
    core_state_replacements = 0
    core_guc_replacements = 0
    core_immutable_references = 0
    external_references = catalog.get("external_references")
    if not isinstance(external_references, list):
        raise ExtensionStatePlanError(
            "catalog: external_references must be an array"
        )
    for index, use in enumerate(external_references):
        if not isinstance(use, dict):
            raise ExtensionStatePlanError(
                f"catalog: external_references[{index}] must be an object"
            )
        identifier = use.get("id")
        name = use.get("name")
        if (
            not isinstance(identifier, str)
            or not identifier.startswith("external:")
            or not isinstance(name, str)
            or identifier != f"external:{name}"
        ):
            raise ExtensionStatePlanError(
                f"catalog: external_references[{index}] has invalid identity"
            )
        decision = core_decisions.get(identifier)
        if decision is None:
            raise ExtensionStatePlanError(
                f"{identifier}: external mutable state has no kernel ownership decision"
            )
        core_reference_ids.add(identifier)
        owner = decision["owner"]
        if owner == "immutable":
            core_immutable_references += 1
            continue
        if use.get("source_kind") != "source":
            raise ExtensionStatePlanError(
                f"{identifier}: kernel state reference is outside the extension tree"
            )
        if use.get("in_static_initializer"):
            raise ExtensionStatePlanError(
                f"{identifier}: kernel state is embedded in a static initializer"
            )
        path = use.get("path")
        offset = use.get("offset")
        length = use.get("length")
        if (
            not isinstance(path, str)
            or not isinstance(offset, int)
            or not isinstance(length, int)
        ):
            raise ExtensionStatePlanError(
                f"catalog: external_references[{index}] has an invalid source location"
            )
        if owner == "managed_guc":
            guc = guc_by_name.get(name)
            if guc is None:
                raise ExtensionStatePlanError(
                    f"{identifier}: managed GUC has no transform symbol"
                )
            replacement_text = guc["replacement"]
            replacement_kind = "kernel_guc_state"
            core_guc_replacements += 1
        else:
            slot = runtime_by_id.get(identifier)
            if (
                slot is None
                or slot.get("owner") != owner
                or not isinstance(slot.get("enum"), str)
                or slot.get("name") != name
                or slot.get("relocations") not in ([], None)
            ):
                raise ExtensionStatePlanError(
                    f"{identifier}: kernel runtime slot is missing or incompatible"
                )
            replacement_text = (
                f"POSTGAMMA_BACKEND_STATE_VALUE({slot['enum']}, {name})"
            )
            replacement_kind = "kernel_backend_state"
            core_state_replacements += 1
        recorded_use_kind = (
            "macro_body"
            if use.get("macro_kind") == "body"
            else use.get("use_kind", "read")
        )
        replacement = {
            "offset": offset,
            "length": length,
            "original": name,
            "replacement": replacement_text,
            "kind": replacement_kind,
            "rule_id": identifier,
            "use_kind": recorded_use_kind,
            "line": use.get("line", 0),
            "column": use.get("column", 0),
        }
        key = (offset, length)
        existing = file_replacements[path].get(key)
        if existing is not None:
            if any(
                existing[field] != replacement[field]
                for field in ("original", "replacement", "rule_id")
            ):
                raise ExtensionStatePlanError(
                    f"inconsistent semantic binding at {path}:{offset}+{length}"
                )
            if existing["use_kind"] != replacement["use_kind"]:
                existing["use_kind"] = "mixed"
            continue
        file_replacements[path][key] = replacement
        uses_by_kind[recorded_use_kind] += 1

    missing_uses = sorted(role_ids - transformed_ids)
    if missing_uses:
        raise ExtensionStatePlanError(
            "role state has no runtime use(s): " + ", ".join(missing_uses)
        )

    files: list[dict[str, Any]] = []
    replacement_count = 0
    resolved_root = source_root.resolve()
    for path in sorted(file_replacements):
        source = safe_source(resolved_root, path).read_bytes()
        replacements = sorted(
            file_replacements[path].values(),
            key=lambda item: (item["offset"], item["length"], item["rule_id"]),
        )
        previous_end = -1
        for replacement in replacements:
            start = replacement["offset"]
            end = start + replacement["length"]
            if start < previous_end:
                raise ExtensionStatePlanError(
                    f"overlapping extension-state replacements in {path} near {start}"
                )
            expected = replacement["original"].encode("utf-8")
            if source[start:end] != expected:
                raise ExtensionStatePlanError(
                    f"source spelling mismatch in {path} at {start}"
                )
            previous_end = end
        replacement_count += len(replacements)
        files.append(
            {
                "path": path,
                "sha256": hashlib.sha256(source).hexdigest(),
                "replacements": replacements,
            }
        )

    ownership_counts = Counter(
        decisions[identifier]["owner"] for identifier in candidates
    )
    summary = {
        "candidate_count": len(candidates),
        "role_candidate_count": len(role_ids),
        "immutable_candidate_count": ownership_counts["immutable"],
        "file_count": len(files),
        "replacement_count": replacement_count,
        "static_initializer_use_count": 0,
        "dormant_conditional_count": len(dormant),
        "kernel_reference_count": len(external_references),
        "kernel_reference_symbol_count": len(core_reference_ids),
        "kernel_state_replacement_count": core_state_replacements,
        "kernel_guc_replacement_count": core_guc_replacements,
        "kernel_immutable_reference_count": core_immutable_references,
        "by_kind": dict(sorted(uses_by_kind.items())),
    }
    plan = {
        "schema_version": 1,
        "mode": "plan",
        "kind": PLAN_KIND,
        "extension_id": policy["extension_id"],
        "upstream": policy["upstream"],
        "source_root": ".",
        "files": files,
        "summary": summary,
    }
    report = {
        "schema_version": 1,
        "kind": REPORT_KIND,
        "status": "pass",
        "extension_id": policy["extension_id"],
        "upstream": policy["upstream"],
        "accessor": policy["accessor"],
        "summary": summary,
        "candidates": [
            {
                "id": identifier,
                "name": candidates[identifier]["name"],
                "owner": decisions[identifier]["owner"],
                "definition_path": candidates[identifier]["definition_path"],
                "canonical_type": candidates[identifier]["canonical_type"],
                "size": candidates[identifier]["size"],
                "alignment": candidates[identifier]["alignment"],
            }
            for identifier in sorted(candidates)
        ],
        "kernel_references": sorted(core_reference_ids),
    }
    return plan, report


def write_json(path: Path, document: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        json.dumps(document, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--catalog", required=True, type=Path)
    parser.add_argument("--policy", required=True, type=Path)
    parser.add_argument("--source-root", required=True, type=Path)
    parser.add_argument("--selection", required=True, type=Path)
    parser.add_argument("--backend-state-policy", required=True, type=Path)
    parser.add_argument("--backend-state-runtime", required=True, type=Path)
    parser.add_argument("--guc-manifest", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--report", required=True, type=Path)
    args = parser.parse_args()
    try:
        selection = load_json(args.selection, ExtensionStatePlanError)
        if (
            selection.get("schema_version") != 1
            or selection.get("domain") is None
            or selection.get("strategy") != "source-domain"
            or not isinstance(selection.get("selected_translation_units"), int)
            or selection["selected_translation_units"] <= 0
            or selection.get("selected_translation_units")
            != selection.get("domain_translation_units")
        ):
            raise ExtensionStatePlanError("extension AST selection is incomplete")
        plan, report = compile_plan(
            load_json(args.catalog, ExtensionStatePlanError),
            load_json(args.policy, ExtensionStatePlanError),
            args.source_root,
            load_json(args.backend_state_policy, ExtensionStatePlanError),
            load_json(args.backend_state_runtime, ExtensionStatePlanError),
            load_json(args.guc_manifest, ExtensionStatePlanError),
        )
        report["selection"] = {
            "domain": selection["domain"],
            "translation_units": selection["selected_translation_units"],
            "sha256": hashlib.sha256(args.selection.read_bytes()).hexdigest(),
        }
        write_json(args.output, plan)
        write_json(args.report, report)
    except (ExtensionStatePlanError, OSError, KeyError, TypeError) as exc:
        parser.error(str(exc))
    summary = plan["summary"]
    print(
        f"extension state plan: {summary['role_candidate_count']} role slots, "
        f"{summary['replacement_count']} replacements in "
        f"{summary['file_count']} files"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
