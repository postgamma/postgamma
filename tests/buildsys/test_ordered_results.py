"""Tests for the ordered results socket-libpq differential evidence gate."""

from __future__ import annotations

import sys
import unittest
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(PROJECT_ROOT / "buildsys"))

import check_ordered_results  # noqa: E402


def case_output() -> str:
    return "\n".join(
        f"POSTGAMMA_API_CASE name={name} sequence={sequence} kind={kind}"
        for name, sequence, kind in check_ordered_results.EXPECTED_CASES
    )


def candidate_marker() -> str:
    return (
        "POSTGAMMA_API_ORDERED_RESULTS postgres=19 connections=2 "
        "ordered_results=6 prepared_executions=3 text_binary=true "
        "sql_length=true ownership=true transaction_pin=true advisory_pin=true "
        "reuse_after_error=true cancel=true reuse_after_cancel=true "
        "prepared_capability=true phase=closed\n"
    )


def valid_runtime() -> str:
    return (
        "LOG: POSTGAMMA_RUNTIME backend_model=thread host_pid=1234 "
        "provider=pooled threads_started=true role_process_launches=0 "
        "forbidden_process_launch_attempts=0 unsupported_role_requests=0 "
        "role_threads_active=0 client_threads_started=4 pooled_worker_threads=4 "
        "client_quantums=20 quantum_yields=16 carrier_migrations=12 "
        "runnable_sessions=0 running_quantums=0 pinned_sessions=0 "
        "pinned_sessions_peak=1 blocked_sessions=0 execution_tokens_active=0 "
        "execution_token_budget=4 execution_tokens_peak=2 "
        "execution_token_rejections=0\n"
    )


class OrderedResultsEvidenceTests(unittest.TestCase):
    def test_accepts_complete_semantic_sequence(self) -> None:
        cases = check_ordered_results.parse_cases(case_output())
        self.assertEqual(len(cases), 17)

    def test_rejects_reordered_semantic_sequence(self) -> None:
        lines = case_output().splitlines()
        lines[0], lines[1] = lines[1], lines[0]
        with self.assertRaisesRegex(
            check_ordered_results.OrderedResultsCheckError,
            "semantic case sequence mismatch",
        ):
            check_ordered_results.parse_cases("\n".join(lines))

    def test_accepts_candidate_marker(self) -> None:
        values = check_ordered_results.parse_marker(
            candidate_marker(),
            check_ordered_results.CANDIDATE_MARKER,
            candidate=True,
        )
        self.assertEqual(values["prepared_executions"], "3")
        self.assertEqual(values["advisory_pin"], "true")

    def test_accepts_clean_pooled_runtime(self) -> None:
        runtime = check_ordered_results.parse_runtime(valid_runtime())
        self.assertEqual(runtime["host_pid"], 1234)
        self.assertEqual(runtime["pinned_sessions_peak"], 1)

    def test_rejects_missing_pin_observation(self) -> None:
        runtime = valid_runtime().replace(
            "pinned_sessions_peak=1", "pinned_sessions_peak=0"
        )
        with self.assertRaisesRegex(
            check_ordered_results.OrderedResultsCheckError,
            "pinned session",
        ):
            check_ordered_results.parse_runtime(runtime)


if __name__ == "__main__":
    unittest.main()
