"""A source candidate or stale qualification must never activate preparation."""
import copy
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "buildsys"))
from generate_backend_state_preparation import emit, emit_direct
from plan_backend_state_preparation import digest
from qualify_backend_state_preparation import preparation_set_identity
from tests.buildsys import test_state_preparation_qualification as qualification


class PreparationRegistryTests(unittest.TestCase):
    def setUp(self):
        self.fixture = qualification.PreparationQualificationTests()
        self.fixture.setUp()
        self.addCleanup(self.fixture.doCleanups)
        self.report = self.fixture.result()

    def test_registry_is_deterministic_and_uses_linked_provider(self):
        content = emit(self.report)
        self.assertEqual(content, emit(copy.deepcopy(self.report)))
        self.assertIn(self.report["set_identity"], content)
        self.assertIn(self.report["slots"][0]["provider"], content)

    def test_candidates_and_modified_qualification_cannot_activate(self):
        with self.assertRaisesRegex(ValueError, "qualification"):
            emit(self.fixture.plan)
        self.report["slots"][0]["offset"] += 1
        with self.assertRaisesRegex(ValueError, "identity changed"):
            emit(self.report)
        with self.assertRaisesRegex(ValueError, "identity changed"):
            emit_direct(self.report)

    def test_relocation_closure_is_rechecked_before_emission(self):
        self.report["slots"][0]["relocations"] = ["missing"]
        self.report["set_identity"] = preparation_set_identity(self.report)
        self.report["identity"] = digest({k: v for k, v in self.report.items() if k != "identity"})
        with self.assertRaisesRegex(ValueError, "not closed"):
            emit(self.report)


if __name__ == "__main__":
    unittest.main()
