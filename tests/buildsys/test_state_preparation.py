"""Fail closed at initializer, rewrite, dependency and measurement boundaries."""

from __future__ import annotations

import copy
from dataclasses import asdict
import hashlib
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "buildsys"))
import plan_backend_state_preparation as preparation
from generate_backend_state_runtime import compile_slots, relocation_bridge_name


class StatePreparationTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.roots = {kind: self.root / kind for kind in preparation.PLAN_KINDS}
        for root in self.roots.values():
            root.mkdir()
        self.candidates = []
        self.uses = []
        self.edges = []
        self.owners = {}

    def add(self, name, *, initialization="nonzero", function=False, external=False,
            kind="source", owner="role"):
        identifier = ("external:" if external else "internal:") + name
        path = name + ".c"
        definition = path if kind == "source" else "@generated/" + path
        candidate = {
            "id": identifier, "name": name, "usr": "usr:" + identifier,
            "canonical_type": "int", "size": 4, "alignment": 4,
            "has_initializer": initialization != "implicit", "constant_initializer": True,
            "function_static": function, "file_scope": not function,
            "externally_visible": external, "definition_path": path,
            "definition_source_kind": kind, "translation_units": [definition],
            "complete_type": True, "trivially_copyable": True, "tls_kind": "none",
            "use_count": 1, "static_initializer_use_count": 0,
            "preparation": {"language": "c", "target": "x86_64-unknown-linux-gnu",
                            "ordinary_symbol": True, "constant_evaluated": True,
                            "zero_bytes": initialization == "zero"},
        }
        self.candidates.append(candidate)
        self.owners[identifier] = owner
        self.uses.append({"id": identifier, "path": path, "source_kind": kind,
                          "offset": 0, "length": len(name), "in_static_initializer": False})
        (self.roots[kind] / path).write_text(name + ";\n")
        candidate["definition_sha256"] = hashlib.sha256((name + ";\n").encode()).hexdigest()
        return candidate

    def edge(self, owner, target):
        self.edges.append({"owner_id": owner["id"], "target_id": target["id"]})
        kind = owner["definition_source_kind"]
        path = owner["definition_path"]
        file = self.roots[kind] / path
        source = file.read_bytes()
        self.uses.append({"id": target["id"], "in_static_initializer": True,
                          "path": path, "source_kind": kind,
                          "offset": len(source), "length": len(target["name"]),
                          "static_initializer_owner_id": owner["id"]})
        source += (target["name"] + ";\n").encode()
        file.write_bytes(source)
        owner["definition_sha256"] = hashlib.sha256(source).hexdigest()
        target["use_count"] += 1
        target["static_initializer_use_count"] += 1

    def inputs(self):
        catalog = {"schema_version": 1, "kind": "postgamma.backend-mutable-state-catalog",
                   "candidates": self.candidates, "uses": self.uses,
                   "summary": {"candidate_count": len(self.candidates)}}
        policy = {"schema_version": 1, "kind": "postgamma.backend-state-ownership",
                  "decisions": [{"id": c["id"], "owner": self.owners[c["id"]],
                                 "rationale": "fixture"} for c in self.candidates]}
        alignment = {"relocations": self.edges}
        slots, _, _ = compile_slots(catalog, policy, alignment)
        plans = {kind: {"schema_version": 1, "kind": tag, "files": []}
                 for kind, tag in preparation.PLAN_KINDS.items()}
        for slot, candidate in zip(slots, sorted(self.candidates, key=lambda c: c["id"])):
            kind, path = candidate["definition_source_kind"], candidate["definition_path"]
            replacement = (
                f"POSTGAMMA_BACKEND_STATE_RELOCATED_SLOT_VALUE({slot.enum_name}, {slot.name}, "
                f"{relocation_bridge_name(slot.identifier)})" if slot.relocations else
                f"POSTGAMMA_BACKEND_STATE_VALUE({slot.enum_name}, {slot.name})")
            plans[kind]["files"].append({
                "path": path, "sha256": hashlib.sha256((self.roots[kind] / path).read_bytes()).hexdigest(),
                "replacements": [{"kind": "backend_state", "rule_id": slot.identifier,
                                  "offset": 0, "length": len(slot.name),
                                  "original": slot.name, "replacement": replacement}],
            })
        return catalog, policy, alignment, plans, self.roots

    def report(self):
        return preparation.compile_report(*self.inputs())

    def test_general_initializer_routes_do_not_enable_access(self):
        self.add("implicit", initialization="implicit")
        self.add("explicit", initialization="zero", function=True)
        self.add("private")
        self.add("public", external=True)
        self.add("function", function=True)
        self.add("generated", kind="generated_build")
        report = self.report()
        rows = {s["name"]: s for s in report["slots"]}
        self.assertEqual(rows["implicit"]["initialization"], "implicit_zero")
        self.assertEqual(rows["explicit"]["initialization"], "explicit_zero")
        self.assertTrue(rows["explicit"]["copy_template"])  # Existing resolver unchanged.
        self.assertEqual(rows["private"]["template_provider"], "translation_unit_provider")
        self.assertEqual(rows["public"]["template_provider"], "external_address")
        self.assertEqual(rows["function"]["fallback_reasons"], ["function_local_nonzero_template"])
        self.assertTrue(rows["generated"]["preparation_candidate"])
        self.assertEqual(report["summary"]["preparation_candidates"], 5)
        self.assertFalse(any(s["direct_access_eligible"] for s in report["slots"]))
        self.assertEqual(report["evidence"]["non_ast_references"], "not_checked")
        self.assertEqual(report["status"], "analysis_only")

    def test_missing_facts_aliases_qualified_storage_and_cxx_remain_lazy(self):
        for name, change in (
            ("missing", lambda c: c.pop("preparation")),
            ("alias", lambda c: c["preparation"].update(ordinary_symbol=False)),
            ("atomic", lambda c: c.update(atomic=True, volatile=True)),
            ("tls", lambda c: c.update(tls_kind="static")),
            ("cpp", lambda c: c["preparation"].update(language="c++")),
            ("dynamic", lambda c: c.update(constant_initializer=False)),
            ("header", lambda c: c.update(translation_units=["a.c", "b.c"])),
        ):
            change(self.add(name))
        self.assertEqual(self.report()["summary"]["preparation_candidates"], 0)

    def test_volatile_role_storage_preserves_the_same_preparation_proof(self):
        for initialization in ("implicit", "zero", "nonzero"):
            candidate = self.add(initialization, initialization=initialization)
            candidate.update(volatile=True, canonical_type="volatile int")
        report = self.report()
        self.assertEqual(report["summary"]["preparation_candidates"], 3)
        self.assertFalse(any(s["direct_access_eligible"] for s in report["slots"]))
        inputs = self.inputs()
        inputs[3]["source"]["files"][0]["replacements"].clear()
        report = preparation.compile_report(*inputs)
        self.assertEqual(report["summary"]["preparation_candidates"], 2)

    def test_unknown_object_representation_uses_template_and_false_zero_proof_fails(self):
        candidate = self.add("record")
        self.assertEqual(self.report()["slots"][0]["initialization"], "copy_template")
        candidate["preparation"].update(zero_bytes=True, constant_evaluated=False)
        with self.assertRaisesRegex(ValueError, "zero proof"):
            self.report()

    def test_missing_use_rewrite_is_not_a_complete_proof(self):
        self.add("state")
        inputs = self.inputs()
        inputs[3]["source"]["files"][0]["replacements"].clear()
        row = preparation.compile_report(*inputs)["slots"][0]
        self.assertIn("runtime_use_not_rewritten", row["fallback_reasons"])
        self.assertFalse(row["preparation_candidate"])

    def test_stale_source_wrong_binding_or_extra_replacement_is_rejected(self):
        self.add("state")
        inputs = self.inputs()
        (self.roots["source"] / "state.c").write_text("other;\n")
        with self.assertRaisesRegex(ValueError, "stale plan"):
            preparation.compile_report(*inputs)
        (self.roots["source"] / "state.c").write_text("state;\n")
        for field, value in (("rule_id", "external:wrong"), ("replacement", "state")):
            changed = copy.deepcopy(inputs[:4]) + inputs[4:]
            changed[3]["source"]["files"][0]["replacements"][0][field] = value
            with self.subTest(field=field), self.assertRaisesRegex(ValueError, "unmatched"):
                preparation.compile_report(*changed)

    def test_definition_drift_is_rejected_even_if_the_replacement_plan_is_refreshed(self):
        self.add("state")
        (self.roots["source"] / "state.c").write_text("state; /* changed initializer */\n")
        with self.assertRaisesRegex(ValueError, "stale initializer definition"):
            self.report()

    def test_closed_relocation_cycles_and_self_references_are_indivisible(self):
        a, b = self.add("a"), self.add("b")
        self.edge(a, b)
        self.edge(b, a)
        self.edge(a, a)
        report = self.report()
        self.assertEqual(report["summary"]["preparation_candidates"], 2)
        for row in report["slots"]:
            self.assertEqual(row["relocation_component"], [a["id"], b["id"]])
            self.assertEqual(row["rewritten_runtime_uses"], 1)
        b["function_static"] = True
        self.assertEqual(self.report()["summary"]["preparation_candidates"], 0)
        for row in self.report()["slots"]:
            self.assertIn("relocation_component_unqualified", row["fallback_reasons"])

    def test_static_reference_inventory_and_ownership_must_match(self):
        a, b = self.add("a"), self.add("b", owner="session")
        self.edge(a, b)
        with self.assertRaisesRegex(ValueError, "ownership boundary"):
            self.report()
        self.owners[b["id"]] = "role"
        self.edges.clear()
        with self.assertRaisesRegex(ValueError, "static initializer uses"):
            self.report()

    def test_one_macro_token_cannot_be_both_static_initializer_and_runtime(self):
        state = self.add("state")
        self.edge(state, state)
        self.uses[-1]["offset"] = 0
        with self.assertRaisesRegex(ValueError, "static initializer token"):
            self.report()

    def test_supplemental_macro_consumers_count_uses_without_double_counting_tokens(self):
        state = self.add("state")
        self.uses.append({**self.uses[0], "translation_unit": "second.c", "macro_kind": "body"})
        state["use_count"] += 1
        report = self.report()
        self.assertEqual(report["slots"][0]["rewritten_runtime_uses"], 2)
        self.assertTrue(report["slots"][0]["preparation_candidate"])

    def test_profile_coverage_is_conditional_and_requires_matching_identity_and_layout(self):
        self.add("candidate")
        self.add("lazy", function=True)
        inputs = self.inputs()
        slots, _, _ = compile_slots(*inputs[:3])
        profile = {"binary_sha256": "1234", "slot_accesses": [
            {**asdict(s), "accesses": count} for s, count in zip(slots, [90, 10])],
            "runtime_calls": {"relocation_bridge": 10000}}
        coverage = preparation.compile_report(*inputs, profile=profile)["dynamic_coverage"]
        self.assertEqual(coverage["recognized_slot_accesses"], 100)
        self.assertEqual(coverage["candidate_fraction_of_recognized"], 0.9)
        for field, value in (("identifier", "unknown"), ("offset", 999), ("owner", "session")):
            changed = copy.deepcopy(profile)
            changed["slot_accesses"][0][field] = value
            with self.subTest(field=field), self.assertRaises(ValueError):
                preparation.compile_report(*inputs, profile=changed)
        profile["slot_accesses"].append(profile["slot_accesses"][0])
        with self.assertRaisesRegex(ValueError, "duplicate"):
            preparation.compile_report(*inputs, profile=profile)


if __name__ == "__main__":
    unittest.main()
