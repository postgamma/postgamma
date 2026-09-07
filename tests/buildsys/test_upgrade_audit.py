from __future__ import annotations

import copy
import sys
import unittest
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(PROJECT_ROOT / "buildsys"))

import check_backend_state_policy  # noqa: E402
import run_upgrade_audit  # noqa: E402
import upgrade_audit  # noqa: E402


def guc_catalog(*names: str) -> dict[str, object]:
    return {
        "schema_version": 1,
        "kind": "postgamma.postgres-guc-catalog",
        "parameter_count": len(names),
        "parameters": [
            {
                "name": name,
                "storage_symbol": name,
                "value_type": "int",
                "context": "PGC_USERSET",
            }
            for name in names
        ],
    }


def candidate(
    identifier: str,
    name: str,
    usr: str,
    *,
    canonical_type: str = "int",
    definition_path: str = "state.c",
) -> dict[str, object]:
    return {
        "id": identifier,
        "name": name,
        "usr": usr,
        "canonical_type": canonical_type,
        "size": 4 if canonical_type == "int" else 8,
        "alignment": 4 if canonical_type == "int" else 8,
        "array": False,
        "atomic": False,
        "volatile": False,
        "complete_type": True,
        "trivially_copyable": True,
        "externally_visible": identifier.startswith("external:"),
        "file_scope": True,
        "function_static": identifier.startswith("function_static:"),
        "storage_class": "none",
        "tls_kind": "none",
        "definition_kind": "definition",
        "definition_path": definition_path,
        "definition_source_kind": "source",
        "has_initializer": True,
        "constant_initializer": True,
        "initializer_class": "IntegerLiteral",
        "declaration_paths": [definition_path],
        "translation_units": [definition_path],
        "use_count": 1,
        "macro_use_count": 0,
        "static_initializer_use_count": 0,
        "uses_by_kind": {"read": 1},
    }


def state_catalog(*candidates: dict[str, object]) -> dict[str, object]:
    return {
        "schema_version": 1,
        "kind": check_backend_state_policy.CATALOG_KIND,
        "candidates": list(candidates),
        "uses": [],
        "summary": {"candidate_count": len(candidates)},
    }


def inventory(*pairs: tuple[str, str]) -> dict[str, object]:
    return {
        "schema_version": 1,
        "mode": "scan",
        "declarations": [
            {
                "symbol_id": f"pg.guc.{name}",
                "usr": usr,
                "definition": True,
            }
            for name, usr in pairs
        ],
    }


def inheritance_catalog() -> dict[str, object]:
    return {
        "schema_version": 1,
        "kind": "postgamma.backend-inheritance-catalog",
        "contract": {
            "function": "save_backend_variables",
            "translation_unit": "src/backend/postmaster/launch_backend.c",
        },
        "states": [
            {
                "id": "external:PostmasterPid",
                "name": "PostmasterPid",
                "canonical_type": "int",
            }
        ],
    }


def engine_diagnostics() -> dict[str, object]:
    return {
        "shared_implementation": True,
        "baseline_generated_support_anchors": [],
        "candidate_generated_support_anchors": [],
        "baseline_runtime_hooks": [],
        "candidate_runtime_hooks": [],
        "baseline_execution_assumptions": [],
        "candidate_execution_assumptions": [],
    }


class UpgradeAuditTests(unittest.TestCase):
    def test_parses_nul_delimited_worktree_paths_with_spaces(self) -> None:
        document = (
            "worktree /tmp/main\0HEAD abc\0\0"
            "worktree /tmp/candidate with spaces\0HEAD def\0detached\0\0"
        )
        self.assertEqual(
            run_upgrade_audit.worktree_paths(document),
            {Path("/tmp/main"), Path("/tmp/candidate with spaces")},
        )

    def test_usr_offset_drift_is_metadata_not_a_structural_change(self) -> None:
        baseline = state_catalog(candidate("external:stable", "stable", "c:a@1"))
        current = state_catalog(candidate("external:stable", "stable", "c:a@99"))
        result = upgrade_audit.diff_backend_state(baseline, current)
        self.assertEqual(result["structural_changes"], [])
        self.assertEqual(
            [entry["id"] for entry in result["identity_metadata_changes"]],
            ["external:stable"],
        )

    def test_reports_catalog_churn_moves_and_guc_reclassification(self) -> None:
        baseline_state = state_catalog(
            candidate("external:old_guc", "old_guc", "c:@old_guc"),
            candidate(
                "internal:old.c:old.c::moved",
                "moved",
                "c:old.c@moved",
                definition_path="old.c",
            ),
            candidate("external:change", "change", "c:@change"),
        )
        moved = candidate(
            "internal:new.c:new.c::moved",
            "moved",
            "c:new.c@moved",
            definition_path="new.c",
        )
        # Definition paths are intentionally excluded from the move fingerprint.
        moved["definition_path"] = "new.c"
        moved["declaration_paths"] = ["new.c"]
        moved["translation_units"] = ["new.c"]
        candidate_state = state_catalog(
            candidate("external:old_guc", "old_guc", "c:@old_guc"),
            moved,
            candidate(
                "external:change",
                "change",
                "c:@change",
                canonical_type="long",
            ),
        )
        policy = {
            "schema_version": 1,
            "kind": check_backend_state_policy.POLICY_KIND,
            "decisions": [
                {
                    "id": "external:old_guc",
                    "owner": "managed_guc",
                    "rationale": "GUC",
                },
                {"id": "external:change", "owner": "role", "rationale": "role"},
                {
                    "id": "internal:old.c:old.c::moved",
                    "owner": "role",
                    "rationale": "role",
                },
            ],
        }
        adapter = PROJECT_ROOT / "manifests/postgresql/adapter.json"
        report = upgrade_audit.compile_report(
            baseline_upstream={"repository": "example", "commit": "a" * 40},
            candidate_upstream={"repository": "example", "commit": "b" * 40},
            baseline_major=19,
            candidate_major=20,
            baseline_guc_catalog=guc_catalog("old_guc"),
            candidate_guc_catalog=guc_catalog("old_guc", "change"),
            baseline_state_catalog=baseline_state,
            candidate_state_catalog=candidate_state,
            baseline_guc_inventory=inventory(("old_guc", "c:@old_guc")),
            candidate_guc_inventory=inventory(
                ("old_guc", "c:@old_guc"), ("change", "c:@change")
            ),
            baseline_state_policy=policy,
            baseline_inheritance_catalog=inheritance_catalog(),
            candidate_inheritance_catalog=inheritance_catalog(),
            baseline_adapter_path=adapter,
            candidate_adapter_path=adapter,
            engine=engine_diagnostics(),
        )
        self.assertEqual(report["gucs"]["added"], ["change"])
        self.assertEqual(report["compatibility"]["candidate_mode"], "upgrade_probe")
        self.assertEqual(report["compatibility"]["candidate_product_majors"], [19])
        self.assertFalse(
            report["compatibility"]["candidate_is_declared_product_major"]
        )
        self.assertEqual(report["backend_state"]["reused_identity_count"], 2)
        self.assertEqual(len(report["backend_state"]["potential_moves"]), 1)
        self.assertEqual(
            report["policy_reuse"]["reclassified_as_guc"], ["external:change"]
        )
        self.assertEqual(len(report["backend_state"]["structural_changes"]), 1)

    def test_candidate_scan_policy_preserves_known_owners_only(self) -> None:
        baseline = {
            "schema_version": 1,
            "kind": "postgamma.guc-ownership",
            "parameters": {"known": "instance", "removed": "role"},
        }
        policy = run_upgrade_audit.scan_policy(
            guc_catalog("known", "new"), baseline
        )
        self.assertEqual(
            policy["parameters"], {"known": "instance", "new": "session"}
        )
        self.assertIn("not reviewed", policy["purpose"])

    def test_integration_compatibility_separates_product_errors_and_test_warnings(
        self,
    ) -> None:
        result = upgrade_audit.integration_compatibility(
            {
                "baseline_generated_support_anchors": [],
                "candidate_generated_support_anchors": [
                    {
                        "id": "product-edit",
                        "class": "product",
                        "severity": "error",
                        "status": "anchor_mismatch",
                    },
                    {
                        "id": "test-edit",
                        "class": "test",
                        "severity": "warning",
                        "status": "unreadable",
                    },
                ],
                "baseline_runtime_hooks": [],
                "candidate_runtime_hooks": [
                    {
                        "id": "runtime-hook",
                        "class": "runtime_hook",
                        "severity": "error",
                        "status": "incompatible",
                    }
                ],
                "baseline_execution_assumptions": [],
                "candidate_execution_assumptions": [],
            }
        )
        self.assertEqual(result["summary"]["error_count"], 2)
        self.assertEqual(result["summary"]["warning_count"], 1)
        self.assertFalse(result["summary"]["scan_ready"])
        self.assertIsNone(result["summary"]["product_ready"])
        self.assertFalse(result["summary"]["validation_ready"])
        self.assertEqual(
            [entry["id"] for entry in result["findings"]],
            ["product-edit", "test-edit", "runtime-hook"],
        )

    def test_integration_diagnostics_are_required_and_product_readiness_is_unknown(
        self,
    ) -> None:
        incomplete = engine_diagnostics()
        del incomplete["candidate_execution_assumptions"]
        with self.assertRaisesRegex(
            upgrade_audit.UpgradeAuditError, "diagnostics are missing"
        ):
            upgrade_audit.integration_compatibility(incomplete)

        complete = upgrade_audit.integration_compatibility(engine_diagnostics())
        self.assertTrue(complete["summary"]["scan_ready"])
        self.assertIsNone(complete["summary"]["product_ready"])

    def test_integration_diagnostics_reject_malformed_or_duplicate_entries(self) -> None:
        malformed = engine_diagnostics()
        malformed["candidate_runtime_hooks"] = [{"id": "hook"}]
        with self.assertRaisesRegex(
            upgrade_audit.UpgradeAuditError, "invalid status"
        ):
            upgrade_audit.integration_compatibility(malformed)

        duplicate = engine_diagnostics()
        duplicate["candidate_execution_assumptions"] = [
            {"id": "fork", "status": "ok"},
            {"id": "fork", "status": "ok"},
        ]
        with self.assertRaisesRegex(upgrade_audit.UpgradeAuditError, "duplicate id"):
            upgrade_audit.integration_compatibility(duplicate)

        invalid_finding = engine_diagnostics()
        invalid_finding["candidate_generated_support_anchors"] = [
            {"id": "edit", "status": "anchor_mismatch", "severity": "notice"}
        ]
        with self.assertRaisesRegex(
            upgrade_audit.UpgradeAuditError, "invalid severity"
        ):
            upgrade_audit.integration_compatibility(invalid_finding)

    def test_integration_diagnostics_fail_closed_on_an_invalid_baseline(self) -> None:
        engine = engine_diagnostics()
        engine["baseline_execution_assumptions"] = [
            {
                "id": "fork",
                "status": "incompatible",
                "severity": "error",
            }
        ]
        with self.assertRaisesRegex(
            upgrade_audit.UpgradeAuditError, "baseline integration diagnostics"
        ):
            upgrade_audit.integration_compatibility(engine)

    def test_reports_changed_replace_between_region_digest_as_warning(self) -> None:
        engine = engine_diagnostics()
        common = {
            "id": "test-range",
            "class": "test",
            "severity": "warning",
            "status": "ok",
            "mode": "replace_between",
        }
        engine["baseline_generated_support_anchors"] = [
            {**common, "matched_region_sha256": "a" * 64}
        ]
        engine["candidate_generated_support_anchors"] = [
            {**common, "matched_region_sha256": "b" * 64}
        ]
        result = upgrade_audit.integration_compatibility(engine)
        self.assertEqual(result["summary"]["warning_count"], 1)
        self.assertEqual(result["findings"][0]["status"], "region_digest_changed")
        self.assertTrue(result["summary"]["scan_ready"])
        self.assertFalse(result["summary"]["validation_ready"])

    def test_backend_inheritance_drift_is_not_compatible(self) -> None:
        baseline = inheritance_catalog()
        current = copy.deepcopy(baseline)
        current["states"][0]["canonical_type"] = "long"
        current["states"].append(
            {
                "id": "external:MyProc",
                "name": "MyProc",
                "canonical_type": "void *",
            }
        )
        result = upgrade_audit.diff_backend_inheritance(baseline, current)
        self.assertFalse(result["compatible"])
        self.assertEqual(result["added"], ["external:MyProc"])
        self.assertEqual(result["changed"][0]["id"], "external:PostmasterPid")

    def test_normalizes_complete_runtime_hook_diagnostics(self) -> None:
        adapter = {
            "runtime_hooks": [
                {
                    "id": "hook.one",
                    "expected_matches": 1,
                    "source_file_suffixes": ["src/one.c"],
                },
                {"id": "hook.two", "expected_matches": 1},
            ]
        }
        inventory = {
            "injection_diagnostics": [
                {
                    "id": "hook.two",
                    "expected_matches": 1,
                    "actual_matches": 0,
                    "status": "incompatible",
                    "errors": [],
                },
                {
                    "id": "hook.one",
                    "expected_matches": 1,
                    "actual_matches": 1,
                    "status": "ok",
                    "errors": [],
                },
            ]
        }
        result = run_upgrade_audit.runtime_hook_diagnostics(adapter, inventory)
        self.assertEqual([entry["id"] for entry in result], ["hook.one", "hook.two"])
        self.assertEqual(result[0]["source_file_suffixes"], ["src/one.c"])
        self.assertEqual(result[1]["severity"], "error")

    def test_rejects_inconsistent_runtime_hook_diagnostics(self) -> None:
        adapter = {
            "runtime_hooks": [
                {"id": "hook.one", "expected_matches": 1},
            ]
        }
        inventory = {
            "injection_diagnostics": [
                {
                    "id": "hook.one",
                    "expected_matches": 1,
                    "actual_matches": 0,
                    "status": "ok",
                    "errors": [],
                }
            ]
        }
        with self.assertRaisesRegex(
            run_upgrade_audit.UpgradeRunError, "inconsistent status"
        ):
            run_upgrade_audit.runtime_hook_diagnostics(adapter, inventory)

    def test_normalizes_execution_model_assumption_diagnostics(self) -> None:
        adapter = {
            "execution_model_assumptions": [
                {
                    "id": "assume.fork",
                    "callee": "fork",
                    "expected_matches": 1,
                    "allowed_source_file_suffixes": ["src/fork.c"],
                }
            ]
        }
        inventory = {
            "assumption_diagnostics": [
                {
                    "id": "assume.fork",
                    "expected_matches": 1,
                    "actual_matches": 1,
                    "status": "ok",
                    "matches": [
                        {
                            "path": "/candidate/src/fork.c",
                            "line": 10,
                            "column": 2,
                            "allowed": True,
                        }
                    ],
                }
            ]
        }
        result = run_upgrade_audit.execution_assumption_diagnostics(
            adapter, inventory
        )
        self.assertEqual(result[0]["class"], "execution_assumption")
        self.assertEqual(result[0]["status"], "ok")

        inventory["assumption_diagnostics"][0]["matches"][0]["allowed"] = False
        with self.assertRaisesRegex(
            run_upgrade_audit.UpgradeRunError, "inconsistent status"
        ):
            run_upgrade_audit.execution_assumption_diagnostics(adapter, inventory)


if __name__ == "__main__":
    unittest.main()
