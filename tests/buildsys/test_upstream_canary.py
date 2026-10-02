from __future__ import annotations

import sys
import unittest
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(PROJECT_ROOT / "buildsys"))

import run_upstream_canary  # noqa: E402


def stage(identifier: str, status: str) -> dict[str, object]:
    return {"id": identifier, "status": status}


def complete_stages() -> list[dict[str, object]]:
    return [
        stage("reference_build", "passed"),
        stage("candidate_scan", "passed"),
        stage("candidate_facts", "passed"),
        stage("generated_product_build", "passed"),
        stage("threaded_runtime_smoke", "passed"),
        stage("thread_isolation_check", "passed"),
        stage("reference_check_world", "skipped"),
        stage("private_runtime_sanitizers", "skipped"),
        stage("generated_check_world", "skipped"),
        stage("thread_isolation_stress", "skipped"),
    ]


class UpstreamCanaryTests(unittest.TestCase):
    def test_make_arguments_preserve_caller_owned_configuration(self) -> None:
        arguments = run_upstream_canary.make_arguments(
            "make",
            PROJECT_ROOT,
            4,
            {"PG_CONFIGURE_ARGS": "--enable-debug --without-icu"},
            ("reference-build",),
        )
        self.assertIn(
            "PG_CONFIGURE_ARGS=--enable-debug --without-icu", arguments
        )

    def test_candidate_policy_preserves_reviewed_and_marks_new_state(self) -> None:
        catalog = {
            "states": [
                {"id": "external:known"},
                {"id": "external:new"},
            ]
        }
        baseline = {
            "guc_transfer": "postgresql_serialize_restore",
            "startup_data_transfer": "copy_bytes",
            "client_socket_transfer": "move_descriptor_ownership",
            "states": [
                {
                    "id": "external:known",
                    "strategy": "borrow_instance",
                    "rationale": "reviewed",
                },
                {
                    "id": "external:optional",
                    "strategy": "copy_value",
                    "rationale": "reviewed conditional",
                    "availability": "conditional",
                    "condition": "USE_OPTIONAL",
                },
            ],
        }
        policy = run_upstream_canary.candidate_inheritance_policy(catalog, baseline)
        decisions = {entry["id"]: entry for entry in policy["states"]}
        self.assertEqual(decisions["external:known"]["strategy"], "borrow_instance")
        self.assertIn("unreviewed", decisions["external:new"]["rationale"])
        self.assertIn("external:optional", decisions)

    def test_failure_attribution_distinguishes_baseline_and_integration(self) -> None:
        stages = complete_stages()
        stages[0] = stage("reference_build", "failed")
        self.assertEqual(
            run_upstream_canary.classify_stages(stages, "daily")["classification"],
            "upstream_or_toolchain_failure",
        )

        stages = complete_stages()
        stages[3] = stage("generated_product_build", "failed")
        self.assertEqual(
            run_upstream_canary.classify_stages(stages, "daily")["classification"],
            "postgamma_integration_failure",
        )

    def test_daily_success_requires_deterministic_isolation(self) -> None:
        summary = run_upstream_canary.classify_stages(complete_stages(), "daily")
        self.assertEqual(summary["classification"], "compatible")
        self.assertTrue(summary["validation_ready"])

        stages = complete_stages()
        stages[5] = stage("thread_isolation_check", "failed")
        summary = run_upstream_canary.classify_stages(stages, "daily")
        self.assertEqual(summary["classification"], "validation_failure")
        self.assertFalse(summary["validation_ready"])


if __name__ == "__main__":
    unittest.main()
