"""Tests for the fail-closed PostgreSQL backend quantum seam."""

from __future__ import annotations

import shutil
import sys
import tempfile
import unittest
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(PROJECT_ROOT / "buildsys"))

import generate_backend_quantum_plan  # noqa: E402


class BackendQuantumPlanTests(unittest.TestCase):
    def setUp(self) -> None:
        self.manifest_path = (
            PROJECT_ROOT / "manifests/postgresql/backend-quantum.json"
        )
        self.manifest = generate_backend_quantum_plan.load_manifest(
            self.manifest_path
        )

    def test_pg19_quantum_seam_has_two_reviewed_boundaries(self) -> None:
        plan = generate_backend_quantum_plan.compile_plan(
            self.manifest, PROJECT_ROOT / "postgres"
        )
        self.assertEqual(plan["summary"]["replacement_count"], 16)
        self.assertEqual(plan["summary"]["ready_for_query_yield_points"], 1)
        self.assertEqual(plan["summary"]["command_read_yield_points"], 1)
        self.assertEqual(
            {entry["path"] for entry in plan["files"]},
            {
                "src/backend/tcop/postgres.c",
                "src/backend/access/common/printtup.c",
                "src/backend/storage/lmgr/lock.c",
                "src/include/tcop/backend_startup.h",
                "src/include/tcop/tcopprot.h",
            },
        )

    def test_ready_boundary_drift_fails_loudly(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            source_root = Path(temporary)
            for edit in self.manifest["edits"]:
                relative = Path(edit["source_file"])
                target = source_root / relative
                if target.exists():
                    continue
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(PROJECT_ROOT / "postgres" / relative, target)
            postgres = source_root / "src/backend/tcop/postgres.c"
            content = postgres.read_text(encoding="utf-8").replace(
                "ReadyForQuery(whereToSendOutput);",
                "ReadyForQuery(whereToSendOutput /* drift */);",
                1,
            )
            postgres.write_text(content, encoding="utf-8")
            with self.assertRaisesRegex(
                generate_backend_quantum_plan.BackendQuantumPlanError,
                "quantum-ready-yield",
            ):
                generate_backend_quantum_plan.compile_plan(
                    self.manifest, source_root
                )

    def test_resume_flag_survives_postgres_error_longjmp(self) -> None:
        edit = next(
            candidate
            for candidate in self.manifest["edits"]
            if candidate["id"] == "quantum-persistent-locals"
        )
        self.assertIn(
            "volatile bool postgamma_quantum_resumed", edit["replacement"]
        )

    def test_command_read_boundary_precedes_protocol_consumption(self) -> None:
        edit = next(
            candidate
            for candidate in self.manifest["edits"]
            if candidate["id"] == "quantum-command-read-yield"
        )
        self.assertEqual(edit["anchor"], "\t\tDoingCommandRead = true;")
        self.assertEqual(edit["kind"], "insert_after")
        self.assertIn(
            "postgamma_postgres_quantum_yield_command_read",
            edit["replacement"],
        )

    def test_runtime_accounts_for_buffered_protocol_messages(self) -> None:
        source = (
            PROJECT_ROOT / "runtime/src/postgres_backend_runtime.c"
        ).read_text(encoding="utf-8")
        self.assertIn("pq_buffer_remaining_data() > 0", source)

    def test_advisory_lock_accessor_uses_role_local_state(self) -> None:
        edit = next(
            candidate
            for candidate in self.manifest["edits"]
            if candidate["id"] == "advisory-lock-local-hash-role-state"
        )
        self.assertEqual(edit["kind"], "insert_before")
        self.assertIn("POSTGAMMA_BACKEND_STATE_NAMED_VALUE", edit["replacement"])
        self.assertIn("::LockMethodLocalHash", edit["replacement"])
        self.assertNotIn("POSTGAMMA_BACKEND_STATE_SLOT_", edit["replacement"])

    def test_advisory_lock_state_insertions_do_not_cover_ast_state_use(self) -> None:
        plan = generate_backend_quantum_plan.compile_plan(
            self.manifest, PROJECT_ROOT / "postgres"
        )
        source = (
            PROJECT_ROOT / "postgres/src/backend/storage/lmgr/lock.c"
        ).read_bytes()
        token = b"LockMethodLocalHash"
        return_line = b"\treturn LockMethodLocalHash;"
        line_start = source.index(return_line)
        token_start = line_start + return_line.index(token)
        token_end = token_start + len(token)
        replacements = {
            replacement["rule_id"]: replacement
            for file_entry in plan["files"]
            if file_entry["path"] == "src/backend/storage/lmgr/lock.c"
            for replacement in file_entry["replacements"]
        }
        begin = replacements["advisory-lock-local-hash-role-state"]
        end = replacements["advisory-lock-local-hash-role-state-end"]
        self.assertEqual(begin["length"], 0)
        self.assertEqual(begin["offset"], line_start)
        self.assertLess(begin["offset"], token_start)
        self.assertEqual(end["length"], 0)
        self.assertGreater(end["offset"], token_end)

    def test_result_budget_rejects_encoded_rows_before_transport(self) -> None:
        edits = {edit["id"]: edit for edit in self.manifest["edits"]}
        self.assertEqual(
            edits["result-budget-begin"]["enclosing_function"],
            "printtup_startup",
        )
        check = edits["result-budget-check-datarow"]
        self.assertEqual(check["enclosing_function"], "printtup")
        self.assertEqual(check["kind"], "insert_before")
        self.assertEqual(check["anchor"], "\tpq_endmessage_reuse(buf);")
        self.assertIn("postgamma_postgres_result_budget_check", check["replacement"])


if __name__ == "__main__":
    unittest.main()
