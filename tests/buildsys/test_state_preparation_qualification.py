"""Preparation requires the transformed use closure and linked providers."""

from __future__ import annotations

import copy
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "buildsys"))
from qualify_backend_state_preparation import qualify, verify_inputs, merge_audits
from prepare_backend_state_templates import provider_name, install
from tests.buildsys import test_state_preparation as fixtures


class PreparationQualificationTests(unittest.TestCase):
    def setUp(self):
        self.fixture = fixtures.StatePreparationTests()
        self.fixture.setUp()
        self.addCleanup(self.fixture.doCleanups)
        self.fixture.add("state")
        self.plan = self.fixture.report()
        self.catalog = self.fixture.inputs()[0]
        self.units = {"state.c"}
        self.symbols = {provider_name("internal:state")}
        self.audit = copy.deepcopy(self.catalog)
        self.audit["preparation_audit"] = {
            "translation_units": ["state.c"], "opaque_references": [], "inputs": {}}
        self.audit["uses"] = [{"id": "internal:state", "template_use": {
            "kind": "resolver_template", "slot": 0, "function": "resolver"}}]

    def result(self):
        return qualify(self.plan, self.catalog, self.audit, self.units, self.symbols)

    def test_requires_actual_template_provider_linkage(self):
        self.assertEqual(self.result()["summary"]["preparation_eligible"], 1)
        self.symbols.clear()
        self.assertEqual(self.result()["summary"]["preparation_eligible"], 0)

    def test_set_identity_tracks_the_contract_not_audit_reference_counts(self):
        before = self.result()
        self.audit["uses"].append(copy.deepcopy(self.audit["uses"][0]))
        after = self.result()
        self.assertNotEqual(before["identity"], after["identity"])
        self.assertEqual(before["set_identity"], after["set_identity"])
        self.symbols.clear()
        self.assertNotEqual(before["set_identity"], self.result()["set_identity"])

    def test_link_domain_must_be_fully_audited(self):
        self.units.add("new_runtime.c")
        with self.assertRaisesRegex(ValueError, "link closure"):
            self.result()

    def test_raw_read_write_escape_and_wrong_slot_remain_lazy(self):
        for fact in ({"kind": "unconverted"}, {"kind": "resolver_template", "slot": 1},
                     {"kind": "provider_return", "function": "arbitrary_escape"}):
            with self.subTest(fact=fact):
                self.audit["uses"][0]["template_use"] = fact
                self.assertEqual(self.result()["summary"]["preparation_eligible"], 0)

    def test_named_resolver_must_name_the_same_slot(self):
        self.audit["uses"][0]["template_use"] = {
            "kind": "named_resolver_template", "function": "internal:state"}
        self.assertEqual(self.result()["summary"]["preparation_eligible"], 1)
        self.audit["uses"][0]["template_use"]["function"] = "internal:other"
        self.assertEqual(self.result()["summary"]["preparation_eligible"], 0)

    def test_rechecks_zero_facts_and_layout_against_actual_headers(self):
        for field, value in (("size", 8), ("preparation", {})):
            with self.subTest(field=field):
                self.audit["candidates"][0][field] = value
                self.assertEqual(self.result()["summary"]["preparation_eligible"], 0)

    def test_assembly_symbols_and_unbounded_assembly_remain_lazy(self):
        for text, unbounded in (("mov state, %eax", False), (".macro x", True)):
            self.audit["preparation_audit"]["opaque_references"] = [{
                "translation_unit": "state.c", "text": text, "unbounded": unbounded}]
            self.assertEqual(self.result()["summary"]["preparation_eligible"], 0)

    def test_one_bad_member_keeps_the_whole_relocation_component_lazy(self):
        extra = copy.deepcopy(self.plan["slots"][0])
        extra.update(identifier="internal:other", definition="other.c", index=1)
        self.plan["slots"].append(extra)
        for slot in self.plan["slots"]:
            slot["relocation_component"] = ["internal:state", "internal:other"]
        # The missing definition/provider of other cannot leave state qualified.
        self.assertEqual(self.result()["summary"]["preparation_eligible"], 0)

    def test_provider_retains_original_storage_and_rejects_duplicate_install(self):
        source, generated = self.fixture.roots.values()
        receipt = install(self.plan, source, generated)
        self.assertIn("return &(state);", (source / "state.c").read_text())
        self.assertEqual(receipt["providers"]["internal:state"], provider_name("internal:state"))
        with self.assertRaisesRegex(ValueError, "already installed"):
            install(self.plan, source, generated)

    def test_changed_or_missing_inputs_cannot_reuse_qualification(self):
        path = self.fixture.roots["source"] / "state.c"
        with self.assertRaisesRegex(ValueError, "input changed"):
            verify_inputs({str(path): "stale"})
        with self.assertRaisesRegex(ValueError, "empty"):
            verify_inputs({})

    def test_shards_retain_external_uses_and_reject_conflicting_inputs(self):
        first, second = copy.deepcopy(self.audit), copy.deepcopy(self.audit)
        first["preparation_audit"]["inputs"] = {"header.h": "one"}
        second["uses"] = []
        second["candidates"] = []
        second["external_references"] = [{"id": "internal:state", "template_use": {"kind": "unconverted"}}]
        merged = merge_audits([first, second])
        self.assertEqual(qualify(self.plan, self.catalog, merged, self.units, self.symbols)
                         ["summary"]["preparation_eligible"], 0)
        second["preparation_audit"]["inputs"] = {"header.h": "two"}
        with self.assertRaisesRegex(ValueError, "between audit shards"):
            merge_audits([first, second])


if __name__ == "__main__":
    unittest.main()
