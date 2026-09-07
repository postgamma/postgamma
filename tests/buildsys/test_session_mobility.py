"""Tests for the logical-session mobility gate."""

from __future__ import annotations

import copy
import sys
import unittest
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(PROJECT_ROOT / "buildsys"))

import check_session_mobility as checker  # noqa: E402


class SessionMobilityTests(unittest.TestCase):
    @staticmethod
    def catalog() -> dict[str, object]:
        candidates = [
            {
                "id": "external:work_mem",
                "name": "work_mem",
                "usr": "c:@work_mem",
                "canonical_type": "int",
                "definition_path": "guc.c",
                "line": 10,
                "column": 1,
            },
            {
                "id": "external:PG_exception_stack",
                "name": "PG_exception_stack",
                "usr": "c:@PG_exception_stack",
                "canonical_type": "struct __jmp_buf_tag (*)[1]",
                "definition_path": "elog.c",
                "line": 20,
                "column": 2,
            },
        ]
        return {
            "schema_version": 1,
            "kind": checker.CATALOG_KIND,
            "candidates": candidates,
            "uses": [],
            "summary": {"candidate_count": len(candidates)},
        }

    @staticmethod
    def ownership() -> dict[str, object]:
        return {
            "schema_version": 1,
            "kind": checker.OWNERSHIP_KIND,
            "decisions": [
                {
                    "id": "external:work_mem",
                    "owner": "managed_guc",
                    "rationale": "generated GUC storage",
                },
                {
                    "id": "external:PG_exception_stack",
                    "owner": "role",
                    "rationale": "logical backend state",
                },
            ],
        }

    @classmethod
    def mobility(cls) -> dict[str, object]:
        catalog = cls.catalog()
        ownership = cls.ownership()
        defaults = {
            "immutable": ("immutable", "portable"),
            "instance": ("instance", "portable"),
            "managed_guc": ("logical_backend", "portable"),
            "role": ("logical_backend", "portable"),
            "session": ("session", "portable"),
        }
        return {
            "schema_version": 1,
            "kind": checker.MOBILITY_KIND,
            "review_fingerprint": {
                "candidate_ids_sha256": checker.stable_digest(
                    sorted(value["id"] for value in ownership["decisions"])
                ),
                "ownership_decisions_sha256": checker.stable_digest(
                    sorted(ownership["decisions"], key=lambda value: value["id"])
                ),
            },
            "owner_defaults": {
                owner: {
                    "semantic_owner": values[0],
                    "mobility": values[1],
                    "rationale": f"fixture default for {owner}",
                }
                for owner, values in defaults.items()
            },
            "overrides": [
                {
                    "id": "external:PG_exception_stack",
                    "semantic_owner": "logical_backend",
                    "mobility": "carrier_only",
                    "rebind_hook": "fresh_outer_error_boundary",
                    "quiescence": "must_be_null_at_yield",
                    "rationale": "points into a carrier stack",
                }
            ],
            "runtime_carrier_fields": [
                {
                    "name": "exit_jump",
                    "bind_hook": "sigsetjmp",
                    "unbind_hook": "clear_exit_jump_ready",
                    "rationale": "fixture carrier field",
                }
            ],
        }

    @staticmethod
    def guc_inventory() -> dict[str, object]:
        return {
            "schema_version": 1,
            "mode": "scan",
            "declarations": [
                {
                    "definition": True,
                    "symbol_id": "pg.guc.work_mem",
                    "usr": "c:@work_mem",
                }
            ],
        }

    @staticmethod
    def guc_policy() -> dict[str, object]:
        return {
            "schema_version": 1,
            "kind": checker.GUC_POLICY_KIND,
            "parameters": {"work_mem": "session"},
        }

    @staticmethod
    def runtime_source() -> str:
        return (
            "typedef struct PostgammaCarrierFrame\n"
            "{\n"
            "    sigjmp_buf exit_jump;\n"
            "} PostgammaCarrierFrame;\n"
            "void f(void) { sigsetjmp(exit_jump, 1); }\n"
        )

    def validate(self, mobility: dict[str, object] | None = None) -> dict[str, object]:
        return checker.validate_mobility(
            self.catalog(),
            self.ownership(),
            mobility if mobility is not None else self.mobility(),
            self.guc_inventory(),
            self.guc_policy(),
            self.runtime_source(),
        )

    def test_generates_complete_cross_dimension_report(self) -> None:
        report = self.validate()
        self.assertEqual(report["candidate_count"], 2)
        self.assertEqual(report["mobility_counts"]["carrier_only"], 1)
        by_id = {value["id"]: value for value in report["records"]}
        self.assertEqual(by_id["external:work_mem"]["semantic_owner"], "session")
        self.assertEqual(report["missing"], [])
        self.assertEqual(report["stale"], [])

    def test_candidate_change_requires_explicit_review(self) -> None:
        mobility = self.mobility()
        mobility["review_fingerprint"]["candidate_ids_sha256"] = "0" * 64
        with self.assertRaisesRegex(checker.SessionMobilityError, "candidate set changed"):
            self.validate(mobility)

    def test_ownership_change_requires_explicit_review(self) -> None:
        mobility = self.mobility()
        mobility["review_fingerprint"]["ownership_decisions_sha256"] = "1" * 64
        with self.assertRaisesRegex(checker.SessionMobilityError, "ownership decisions changed"):
            self.validate(mobility)

    def test_jump_buffer_cannot_default_to_portable(self) -> None:
        mobility = self.mobility()
        mobility["overrides"] = []
        with self.assertRaisesRegex(checker.SessionMobilityError, "cannot be portable"):
            self.validate(mobility)

    def test_rejects_missing_carrier_field(self) -> None:
        mobility = copy.deepcopy(self.mobility())
        mobility["runtime_carrier_fields"][0]["name"] = "missing_field"
        with self.assertRaisesRegex(checker.SessionMobilityError, "missing reviewed carrier"):
            self.validate(mobility)


if __name__ == "__main__":
    unittest.main()
