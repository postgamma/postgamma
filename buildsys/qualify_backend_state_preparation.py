#!/usr/bin/env python3
"""Qualify preparation for one compiled native server and its audited consumers.

The source candidate report alone is never authorization to prepare a slot.
Reparse the transformed compilation domain, check every surviving template use,
and intersect candidates with the actual kernel link closure. Unknown uses and
incomplete relocation components remain lazy. This does not enable hot access.
"""

from __future__ import annotations

import argparse
from collections import Counter
from concurrent.futures import ThreadPoolExecutor
import copy
import hashlib
import json
from pathlib import Path
import re
import subprocess

from filter_compile_database import compiler_output, record_arguments, select_compile_records
from generate_backend_state_runtime import relocation_bridge_name
from merge_link_db import command_facts
from plan_backend_state_preparation import digest
from prepare_backend_state_templates import provider_name, provider_ids


def file_digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def unit_name(path: Path, source: Path, build: Path) -> str:
    for root, prefix in ((source, ""), (build, "@generated/")):
        try:
            return prefix + path.resolve().relative_to(root.resolve()).as_posix()
        except ValueError:
            pass
    raise ValueError(f"compilation input outside audited roots: {path}")


def verify_inputs(inputs: dict) -> None:
    if not inputs:
        raise ValueError("empty preparation audit input ledger")
    for path, expected in inputs.items():
        if file_digest(Path(path)) != expected:
            raise ValueError(f"preparation audit input changed: {path}")


def merge_audits(parts: list[dict]) -> dict:
    definitions, uses, inputs, units, opaque = {}, [], {}, set(), []
    for part in parts:
        for candidate in part["candidates"]:
            previous = definitions.get(candidate["id"])
            if previous is not None:
                for field in ("canonical_type", "size", "alignment", "preparation",
                              "has_initializer", "constant_initializer"):
                    if previous.get(field) != candidate.get(field):
                        raise ValueError(f"conflicting audit definition: {candidate['id']}")
            else:
                definitions[candidate["id"]] = candidate
        uses.extend([*part["uses"], *part.get("external_references", [])])
        proof = part["preparation_audit"]
        for path, value in proof["inputs"].items():
            if path in inputs and inputs[path] != value:
                raise ValueError(f"input changed between audit shards: {path}")
            inputs[path] = value
        units.update(proof["translation_units"])
        opaque.extend(proof["opaque_references"])
    return {"candidates": [definitions[k] for k in sorted(definitions)], "uses": uses,
            "preparation_audit": {"inputs": inputs, "translation_units": sorted(units),
                                  "opaque_references": opaque}}


def link_closure(fragment: dict, commands: list, source: Path, build: Path) -> tuple[set, dict, list]:
    directory = Path(fragment["directory"])
    facts = command_facts(fragment["arguments"], directory)
    outputs, records = {}, {}
    for command in commands:
        output = compiler_output(record_arguments(command), Path(command["directory"]))
        if output is not None:
            unit = unit_name(Path(command["file"]), source, build)
            if output in outputs and outputs[output] != unit:
                raise ValueError("ambiguous object source in compile database")
            outputs[output] = unit
            if output in records and record_arguments(records[output]) != record_arguments(command):
                raise ValueError(f"ambiguous object compilation: {output}")
            records[output] = command
    units, inputs, linked_records = set(), {}, {}
    for item in facts["ordered_inputs"]:
        path = Path(item["path"])
        inputs[str(path)] = file_digest(path)
        if item["kind"] == "archive":
            members = subprocess.check_output(["ar", "t", str(path)], text=True).splitlines()
            objects = [path.parent / member for member in members]
        elif item["kind"] == "object":
            objects = [path]
        else:
            raise ValueError("opaque link inputs require a separate preparation audit")
        for obj in objects:
            if obj.resolve() not in outputs:
                raise ValueError(f"link object has no audited compilation: {obj}")
            units.add(outputs[obj.resolve()])
            linked_records[obj.resolve()] = records[obj.resolve()]
    if not units:
        raise ValueError("empty kernel link closure")
    return units, inputs, list(linked_records.values())


def preparation_set_identity(report: dict) -> str:
    """The runtime set is independent of diagnostics for fallback slots."""
    fields = ("identifier", "index", "enum_name", "owner", "local_index", "offset",
              "size", "alignment", "initialization", "provider", "relocations")
    return digest({"layout": report["layout_sha256"], "entries": [
        {key: slot[key] for key in fields}
        for slot in report["slots"] if slot["preparation_eligible"]]})


def qualify(plan: dict, catalog: dict, audit: dict, linked_units: set[str],
            linked_symbols: set[str]) -> dict:
    if plan.get("kind") != "postgamma.backend-state-preparation-plan":
        raise ValueError("expected preparation candidate plan")
    proof = audit.get("preparation_audit")
    if not proof or not set(proof["translation_units"]) >= linked_units:
        raise ValueError("audit does not cover the kernel link closure")
    original = {c["id"]: c for c in catalog["candidates"]}
    current = {c["id"]: c for c in audit["candidates"]}
    entries = {e["identifier"]: copy.deepcopy(e) for e in plan["slots"]}
    required_providers = provider_ids(plan)
    bridges = {relocation_bridge_name(i): e for i, e in entries.items() if e["relocations"]}
    for identifier, entry in entries.items():
        reasons = entry["fallback_reasons"]
        if entry["definition"] not in linked_units:
            reasons.append("definition_not_in_kernel_link_closure")
        observed = current.get(identifier)
        if not observed:
            reasons.append("definition_not_in_transformed_ast")
        else:
            # Re-evaluate against the *actual* configured headers and transformed
            # source. Matching old source text is not enough for sizeof/ABI/zero.
            for field in ("canonical_type", "size", "alignment", "preparation",
                          "constant_initializer", "has_initializer", "tls_kind",
                          "volatile", "atomic", "file_scope", "function_static"):
                if observed.get(field) != original[identifier].get(field):
                    reasons.append("transformed_initializer_or_layout_differs")
                    break
        if identifier in required_providers and provider_name(identifier) not in linked_symbols:
            reasons.append("template_provider_not_linked")
        entry["audited_template_uses"] = Counter()

    for use in [*audit["uses"], *audit.get("external_references", [])]:
        entry = entries.get(use["id"])
        if entry is None:
            continue
        fact = use.get("template_use", {})
        kind = fact.get("kind", "unconverted")
        valid = kind == "unevaluated"
        if kind == "resolver_template":
            valid = fact["slot"] == entry["index"]
        elif kind == "named_resolver_template":
            valid = fact["function"] == use["id"]
        elif kind == "provider_return":
            valid = (use["id"] in required_providers and
                     fact["function"] == provider_name(use["id"]))
        elif kind == "static_initializer":
            owner = entries.get(use.get("static_initializer_owner_id"))
            bridge = bridges.get(fact.get("function"))
            valid = ((owner is not None and use["id"] in owner["relocations"]) or
                     (bridge is not None and use["id"] in bridge["relocations"]))
        entry["audited_template_uses"][kind] += 1
        if not valid:
            entry["fallback_reasons"].append("unconverted_template_use")

    opaque_units = {}
    all_tokens, any_unbounded = set(), False
    for opaque in proof["opaque_references"]:
        # Extended asm operands are ordinary AST references and were checked
        # above. Literal symbol names/aliases are checked separately. Basic asm,
        # assembler directives and token construction have no bounded operands.
        tokens = set(re.findall(r"[A-Za-z_][A-Za-z_0-9]*", opaque["text"]))
        unbounded = opaque["unbounded"] or bool(re.search(
            r"(^|[;\n])\s*\.[A-Za-z_]|\\|##", opaque["text"]))
        previous_tokens, previous_unbounded = opaque_units.get(opaque["translation_unit"], (set(), False))
        opaque_units[opaque["translation_unit"]] = (previous_tokens | tokens, previous_unbounded or unbounded)
        all_tokens |= tokens
        any_unbounded |= unbounded
    for identifier, entry in entries.items():
        tokens, unbounded = ((all_tokens, any_unbounded) if identifier.startswith("external:")
                             else opaque_units.get(entry["definition"], (set(), False)))
        if unbounded or entry["name"] in tokens:
            entry["fallback_reasons"].append("opaque_symbol_reference")

    for entry in entries.values():
        if any(entries[i]["fallback_reasons"] for i in entry["relocation_component"]):
            entry["fallback_reasons"].append("relocation_component_unqualified")
    for entry in entries.values():
        entry["fallback_reasons"] = sorted(set(entry["fallback_reasons"]))
        entry["preparation_eligible"] = not entry["fallback_reasons"]
        # S4 is deliberately separate. Neither a source report nor this audit
        # changes POSTGAMMA_BACKEND_STATE_VALUE's access contract.
        entry["direct_access_eligible"] = False
        entry["provider"] = provider_name(entry["identifier"]) if entry["identifier"] in required_providers else None
    result = {
        "schema_version": 1, "kind": "postgamma.backend-state-preparation-qualified",
        "scope": "audited_native_server_configuration", "layout_sha256": plan["layout_sha256"],
        "slots": list(entries.values()),
        "summary": {
            "preparation_eligible": sum(e["preparation_eligible"] for e in entries.values()),
            "direct_access_eligible": 0,
            "fallback_reasons": dict(Counter(r for e in entries.values() for r in e["fallback_reasons"])),
        },
    }
    result["set_identity"] = preparation_set_identity(result)
    result["identity"] = digest(result)
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("plan", "catalog", "compile-database", "link-fragment", "source-root",
                 "generated-root", "domain", "inventory-tool", "output"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--jobs", type=int, default=1)
    args = parser.parse_args()
    try:
        read = lambda path: json.loads(path.read_text())
        source, build = args.source_root.resolve(), args.generated_root.resolve()
        if args.jobs < 1:
            raise ValueError("jobs must be positive")
        commands = read(args.compile_database)
        fragment = read(args.link_fragment)
        linked_units, link_inputs, linked_commands = link_closure(fragment, commands, source, build)
        domain = read(args.domain)
        selected, _ = select_compile_records(commands, source, build, domain)
        # Some common backend objects deliberately define FRONTEND. They are
        # outside the mutation inventory but still inside the actual link.
        selected_keys = {(c["file"], tuple(record_arguments(c))) for c in selected}
        for command in linked_commands:
            key = (command["file"], tuple(record_arguments(command)))
            if unit_name(Path(command["file"]), source, build) in linked_units and key not in selected_keys:
                selected.append(command)
                selected_keys.add(key)
        work = args.output.parent / (args.output.stem + "-audit")
        work.mkdir(parents=True, exist_ok=True)
        database = work / "compile_commands.json"
        database.write_text(json.dumps(selected, indent=2) + "\n")
        output = work / "inventory.json"
        files = sorted({c["file"] for c in selected})
        jobs = min(args.jobs, len(files))
        def scan(index):
            shard = work / f"inventory-{index}.json"
            argv = [str(args.inventory_tool.resolve()), "--audit-preparation",
                    f"--source-root={source}", f"--generated-root={build}",
                    f"--definition-domain={args.domain.resolve()}", f"--output={shard}",
                    "-p", str(work), *files[index::jobs]]
            with (work / f"inventory-{index}.log").open("w") as log:
                subprocess.run(argv, stdout=log, stderr=subprocess.STDOUT, check=True)
            return read(shard)
        with ThreadPoolExecutor(max_workers=jobs) as pool:
            audit = merge_audits(list(pool.map(scan, range(jobs))))
        output.write_text(json.dumps(audit, sort_keys=True) + "\n")
        verify_inputs(audit["preparation_audit"]["inputs"])
        binary = Path(fragment["output"])
        symbols = subprocess.check_output(["nm", "--defined-only", str(binary)], text=True)
        qualified = qualify(read(args.plan), read(args.catalog), audit, linked_units,
                            {line.split()[-1] for line in symbols.splitlines() if line.split()})
        qualified["evidence"] = {
            "compile_database": file_digest(args.compile_database),
            "link_fragment": file_digest(args.link_fragment), "link_inputs": link_inputs,
            "binary": {str(binary): file_digest(binary)},
            "inventory_tool": file_digest(args.inventory_tool),
            "audit": {str(output): file_digest(output)},
            "inputs": audit["preparation_audit"]["inputs"],
            "audited_translation_units": audit["preparation_audit"]["translation_units"],
            "linked_translation_units": sorted(linked_units),
        }
        args.output.write_text(json.dumps(qualified, indent=2, sort_keys=True) + "\n")
        print("preparation qualification: " + json.dumps(qualified["summary"], sort_keys=True))
    except (ValueError, OSError, KeyError, subprocess.CalledProcessError) as exc:
        parser.error(str(exc))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
