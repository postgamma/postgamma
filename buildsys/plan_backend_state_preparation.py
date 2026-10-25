#!/usr/bin/env python3
"""Audit preparation candidates; this report never enables direct access.

The AST/plan proof is necessary but not sufficient: installed generated code,
untransformed consumers and provider linkage must be checked before activation.
Keep those gaps explicit instead of treating an absent reference as evidence.
"""

from __future__ import annotations

import argparse
from collections import Counter, defaultdict
from dataclasses import asdict
import hashlib
import json
from pathlib import Path
from typing import Any

from check_backend_state_policy import catalog_candidates, load_json, validate_alignment
from generate_backend_state_runtime import compile_slots, relocation_bridge_name
from generate_backend_state_plan import safe_source


PLAN_KINDS = {
    "source": "postgamma.backend-state-plan",
    "generated_build": "postgamma.backend-generated-state-plan",
}
ACTIVATION_GATES = [
    "verify_catalog_build_inputs_and_configuration",
    "verify_applied_generated_artifacts",
    "audit_untransformed_consumers_and_non_ast_references",
    "link_explicit_template_providers",
    "prepare_after_inheritance_and_publish_initialized_before_binding",
]


def digest(document: Any) -> str:
    return hashlib.sha256(json.dumps(document, sort_keys=True,
                                    separators=(",", ":")).encode()).hexdigest()


def checked_replacements(plans: dict, roots: dict) -> dict:
    """Validate source identity and bind each replacement to its exact token."""
    result = {}
    for kind, expected_kind in PLAN_KINDS.items():
        plan = plans[kind]
        if plan.get("schema_version") != 1 or plan.get("kind") != expected_kind:
            raise ValueError(f"invalid {kind} state plan")
        files = set()
        for entry in plan["files"]:
            path = entry["path"]
            if path in files:
                raise ValueError(f"duplicate plan file: {kind}:{path}")
            files.add(path)
            source = safe_source(roots[kind].resolve(), path).read_bytes()
            if hashlib.sha256(source).hexdigest() != entry["sha256"]:
                raise ValueError(f"stale plan source: {kind}:{path}")
            for replacement in entry["replacements"]:
                if replacement["kind"] != "backend_state":
                    continue
                offset, length = replacement["offset"], replacement["length"]
                if (type(offset) is not int or type(length) is not int or offset < 0
                        or length <= 0 or source[offset:offset + length] !=
                        replacement["original"].encode()):
                    raise ValueError(f"invalid replacement token: {kind}:{path}")
                key = (kind, path, offset, length)
                if key in result:
                    raise ValueError(f"duplicate replacement: {key}")
                result[key] = replacement
    return result


def compile_report(catalog: dict, policy: dict, alignment: dict,
                   plans: dict, roots: dict, profile: dict | None = None) -> dict:
    candidates = catalog_candidates(catalog)
    slots, sizes, counts = compile_slots(catalog, policy, alignment)
    by_id = {s.identifier: s for s in slots}
    replacements = checked_replacements(plans, roots)
    uses = defaultdict(list)
    observed_edges = set()
    for use in catalog["uses"]:
        if use["id"] in by_id:
            uses[use["id"]].append(use)
            if use.get("in_static_initializer"):
                key = (use.get("source_kind", "source"), use["path"],
                       use["offset"], use["length"])
                if key in replacements:
                    raise ValueError("static initializer token would be rewritten")
                observed_edges.add((use.get("static_initializer_owner_id"), use["id"]))
    declared_edges = {(s.identifier, target) for s in slots for target in s.relocations}
    if observed_edges != declared_edges:
        raise ValueError("relocation evidence does not match static initializer uses")
    for owner, target in declared_edges:
        if owner not in by_id or by_id[owner].owner != by_id[target].owner:
            raise ValueError("relocation crosses an ownership boundary")

    entries = {}
    consumed = set()
    definition_hashes = {}
    for slot in slots:
        identifier = slot.identifier
        candidate = candidates[identifier]
        facts = candidate.get("preparation", {})
        reasons = []
        source_kind = candidate.get("definition_source_kind", "source")
        definition_key = (source_kind, candidate["definition_path"])
        if "definition_sha256" not in candidate:
            reasons.append("definition_source_identity_missing")
        else:
            if definition_key not in definition_hashes:
                path = safe_source(roots[source_kind].resolve(), candidate["definition_path"])
                definition_hashes[definition_key] = hashlib.sha256(path.read_bytes()).hexdigest()
            if candidate["definition_sha256"] != definition_hashes[definition_key]:
                raise ValueError(f"stale initializer definition: {identifier}")
        if slot.owner != "role":
            reasons.append("owner_is_not_role")
        if (candidate.get("use_count") != len(uses[identifier]) or
                candidate.get("static_initializer_use_count") !=
                sum(bool(u.get("in_static_initializer")) for u in uses[identifier])):
            raise ValueError(f"inconsistent use inventory: {identifier}")
        if not facts:
            reasons.append("initializer_facts_missing")
        elif facts.get("language") != "c":
            reasons.append("cxx_initialization_requires_separate_proof")
        if facts.get("ordinary_symbol") is not True:
            reasons.append("symbol_identity_unproved")
        # Volatile qualifies accesses, not the storage owner. The generated
        # lvalue retains typeof(&original), including volatile; initialization
        # still finishes before binding and publishing the role. Atomic objects
        # and native TLS require different initialization/lifetime proofs.
        if candidate.get("atomic") or candidate.get("tls_kind") != "none":
            reasons.append("qualified_or_native_tls_storage")
        if not candidate.get("complete_type") or not candidate.get("trivially_copyable"):
            reasons.append("object_copy_unproved")

        # Preserve the existing implicit-zero storage contract; only explicit
        # initializers need an additional byte representation proof.
        initialization = ("implicit_zero" if not slot.copy_template else
                          "explicit_zero" if facts.get("zero_bytes") is True else
                          "copy_template")
        if initialization == "explicit_zero" and (
                facts.get("constant_evaluated") is not True or
                not candidate.get("constant_initializer")):
            raise ValueError(f"zero proof lacks constant evaluation: {identifier}")
        provider = "none"
        if initialization == "copy_template":
            if not candidate.get("constant_initializer") or not facts.get("constant_evaluated"):
                reasons.append("constant_template_unproved")
            if candidate.get("function_static"):
                provider = "unavailable_function_scope"
                reasons.append("function_local_nonzero_template")
            elif candidate.get("externally_visible"):
                provider = "external_address"
            elif candidate.get("file_scope"):
                provider = "translation_unit_provider"
            else:
                provider = "unavailable_scope"
                reasons.append("template_scope_unproved")
        definition = candidate["definition_path"]
        if candidate.get("definition_source_kind", "source") == "generated_build":
            definition = "@generated/" + definition
        if candidate.get("translation_units") != [definition]:
            reasons.append("definition_is_not_unique_translation_unit_local")

        runtime_uses = 0
        matched_uses = 0
        for use in uses[identifier]:
            if use.get("in_static_initializer"):
                continue  # The whole relocation component is checked below.
            runtime_uses += 1
            key = (use.get("source_kind", "source"), use["path"],
                   use["offset"], use["length"])
            replacement = replacements.get(key)
            expected = (f"POSTGAMMA_BACKEND_STATE_RELOCATED_SLOT_VALUE({slot.enum_name}, {slot.name}, "
                        f"{relocation_bridge_name(identifier)})" if slot.relocations else
                        f"POSTGAMMA_BACKEND_STATE_VALUE({slot.enum_name}, {slot.name})")
            if (replacement is None or replacement["rule_id"] != identifier or
                    replacement["replacement"] != expected or
                    replacement["original"] != slot.name):
                reasons.append("runtime_use_not_rewritten")
            else:
                consumed.add(key)
                matched_uses += 1
        if not runtime_uses:
            reasons.append("no_runtime_use_evidence")
        entries[identifier] = {
            **asdict(slot), "initialization": initialization,
            "initializer_facts": facts, "template_provider": provider,
            "definition": definition,
            "preparation_boundary": "after_inheritance_before_first_bind",
            "runtime_uses": runtime_uses, "rewritten_runtime_uses": matched_uses,
            "static_initializer_uses": len(uses[identifier]) - runtime_uses,
            "fallback_reasons": sorted(set(reasons)),
            "relocation_component": [],
        }
    if set(replacements) - consumed:
        # Unknown/stale replacements cannot count as successful coverage.
        raise ValueError("state plan contains unmatched runtime replacements")

    # Treat incoming and outgoing references together. An unqualified member
    # keeps the complete component lazy, including cycles and self-reference.
    neighbors = defaultdict(set)
    for owner, target in declared_edges:
        neighbors[owner].add(target)
        neighbors[target].add(owner)
    remaining = set(neighbors)
    while remaining:
        pending = [min(remaining)]
        component = set()
        while pending:
            current = pending.pop()
            if current in component:
                continue
            component.add(current)
            pending.extend(neighbors[current] - component)
        remaining -= component
        blocked = any(entries[i]["fallback_reasons"] for i in component)
        for identifier in sorted(component):
            entries[identifier]["relocation_component"] = sorted(component)
            if blocked:
                entries[identifier]["fallback_reasons"].append("relocation_component_unqualified")

    for entry in entries.values():
        entry["preparation_candidate"] = not entry["fallback_reasons"]
        # Deliberately impossible to confuse a source-plan candidate with a
        # production proof or a release gate that authorizes direct access.
        entry["direct_access_eligible"] = False
    report = {
        "schema_version": 1, "kind": "postgamma.backend-state-preparation-plan",
        "status": "analysis_only", "activation_gates": ACTIVATION_GATES,
        "evidence": {"rewrite": "source_hash_and_exact_replacement_plan",
                     "catalog_configuration": "not_checked",
                     "generated_artifacts": "not_checked",
                     "untransformed_consumers": "not_checked",
                     "non_ast_references": "not_checked"},
        "input_sha256": {"catalog": digest(catalog), "policy": digest(policy),
                         "alignment": digest(alignment), "plans": digest(plans)},
        "layout_sha256": digest([asdict(s) for s in slots]),
        "sizes": sizes, "counts": counts, "slots": list(entries.values()),
        "summary": {
            "preparation_candidates": sum(e["preparation_candidate"] for e in entries.values()),
            "direct_access_eligible": 0,
            "initialization": dict(sorted(Counter(e["initialization"] for e in entries.values()).items())),
            "fallback_reasons": dict(sorted(Counter(
                reason for e in entries.values() for reason in e["fallback_reasons"]).items())),
        },
    }
    if profile is not None:
        report["dynamic_coverage"] = dynamic_coverage(entries, profile)
    return report


def dynamic_coverage(entries: dict, profile: dict) -> dict:
    """Join by stable identity *and* layout, never by variable basename."""
    totals = Counter()
    seen = set()
    for access in profile["slot_accesses"]:
        identifier = access["identifier"]
        if identifier in seen or identifier not in entries:
            raise ValueError("profile has duplicate or unknown slot identity")
        seen.add(identifier)
        entry = entries[identifier]
        for field in ("index", "owner", "offset", "size", "alignment"):
            if access[field] != entry[field]:
                raise ValueError(f"profile layout differs for {identifier}: {field}")
        count = access["accesses"]
        if type(count) is not int or count < 0:
            raise ValueError("invalid profile access count")
        totals["recognized_slot_accesses"] += count
        if entry["preparation_candidate"]:
            totals["candidate_accesses"] += count
            totals[entry["initialization"]] += count
    total = totals["recognized_slot_accesses"]
    return {**totals, "candidate_fraction_of_recognized":
            totals["candidate_accesses"] / total if total else None,
            "profile_sha256": digest(profile),
            "binary_sha256": profile["binary_sha256"],
            "limitations": [
                "Candidate coverage is conditional on every activation gate.",
                "Denominator includes only recognized slot loads, not all accesses or instructions.",
                "Dynamic/unclassified accesses and relocation bridge calls are not added.",
            ]}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("catalog", "policy", "guc-inventory", "review-rules",
                 "source-plan", "generated-plan", "source-root", "generated-root", "output"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--profile", type=Path)
    args = parser.parse_args()
    try:
        catalog, policy = load_json(args.catalog), load_json(args.policy)
        alignment = validate_alignment(catalog, policy, load_json(args.guc_inventory),
                                       load_json(args.review_rules))
        report = compile_report(
            catalog, policy, alignment,
            {"source": load_json(args.source_plan), "generated_build": load_json(args.generated_plan)},
            {"source": args.source_root, "generated_build": args.generated_root},
            load_json(args.profile) if args.profile else None)
    except (ValueError, OSError, KeyError, TypeError) as exc:
        parser.error(str(exc))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print("backend state preparation: " + json.dumps(report["summary"], sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
