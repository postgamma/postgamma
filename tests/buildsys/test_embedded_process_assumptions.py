"""Tests for the fail-closed embedded process-assumption inventory."""

from __future__ import annotations

import sys
import tempfile
import unittest
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(PROJECT_ROOT / "buildsys"))

import check_embedded_process_assumptions  # noqa: E402
import postgresql_embedded_adapter  # noqa: E402


class EmbeddedProcessAssumptionTests(unittest.TestCase):
    def test_pg19_inventory_classifies_every_project_process_call(self) -> None:
        adapter = postgresql_embedded_adapter.load_adapter(
            PROJECT_ROOT / "manifests/postgresql/embedded.json"
        )
        keys: set[tuple[str, str, int]] = set()
        categories: set[str] = set()
        for assumption in adapter["process_assumptions"]:
            facts, record_keys = (
                check_embedded_process_assumptions.assumption_facts(
                    assumption, PROJECT_ROOT, PROJECT_ROOT / "postgres"
                )
            )
            self.assertEqual(facts["status"], "classified")
            categories.add(facts["category"])
            keys.update(record_keys)
        self.assertEqual(
            keys,
            check_embedded_process_assumptions.project_process_calls(
                PROJECT_ROOT
            ),
        )
        self.assertEqual(
            categories,
            check_embedded_process_assumptions.REQUIRED_CATEGORIES,
        )

    def test_function_bounded_probe_fails_on_anchor_drift(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "runtime/src/probe.c"
            source.parent.mkdir(parents=True)
            source.write_text(
                "void expected(void);\n"
                "void\nprobe(void)\n{\n\texpected();\n}\n",
                encoding="utf-8",
            )
            assumption = {
                "id": "fixture.drift",
                "category": "signal_emission",
                "source_root": "project",
                "source_file": "runtime/src/probe.c",
                "scope": "probe",
                "probe_kind": "call",
                "probe": "expected",
                "expected_matches": 2,
                "classification": "required_for_embedded_kernel",
                "rationale": "fixture",
            }
            with self.assertRaisesRegex(
                check_embedded_process_assumptions.ProcessAssumptionError,
                "expected 2 match",
            ):
                check_embedded_process_assumptions.assumption_facts(
                    assumption, root, root
                )


if __name__ == "__main__":
    unittest.main()
