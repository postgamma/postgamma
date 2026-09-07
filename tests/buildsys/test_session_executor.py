"""Tests for the session execution bounded session/executor evidence validator."""

from __future__ import annotations

import sys
import unittest
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(PROJECT_ROOT / "buildsys"))

import check_session_executor  # noqa: E402


def valid_runtime() -> str:
    return (
        "LOG: POSTGAMMA_RUNTIME backend_model=thread provider=pooled "
        "threads_started=true role_process_launches=0 "
        "forbidden_process_launch_attempts=0 unsupported_role_requests=0 "
        "client_threads_started=4 client_threads_peak=4 "
        "parallel_threads_started=3 role_completions=1008 role_threads_active=0 "
        "pooled_worker_threads=4 client_quantums=3012 quantum_yields=2012 "
        "carrier_migrations=1800 work_steals=4 runnable_sessions=0 "
        "runnable_sessions_peak=996 running_quantums=0 running_quantums_peak=4 "
        "pinned_sessions=0 pinned_sessions_peak=4 blocked_sessions=0 "
        "blocked_sessions_peak=3 execution_tokens_active=0 "
        "execution_tokens_peak=4 execution_token_budget=4 "
        "execution_token_rejections=1 queue_wait_ns_total=1000 "
        "queue_wait_ns_max=100\n"
    )


class SessionExecutorEvidenceTests(unittest.TestCase):
    def test_accepts_bounded_product_evidence(self) -> None:
        marker = check_session_executor.parse_marker(
            "POSTGAMMA_SESSION_EXECUTOR connections=1000 "
            "idle_thread_growth=0 idle_fd_growth=4000 nofile_soft=16384 "
            "request_thread_growth=0 request_storm=1000 "
            "holder_progress=true saturated_waiters=4 pinning_cliff=true "
            "pinned_transactions=4 queued_at_cliff=true parallel_query=true "
            "executor_workers=4 request_threads=0 phase=closed\n"
        )
        runtime = check_session_executor.parse_runtime(valid_runtime())
        self.assertEqual(marker["connections"], "1000")
        self.assertEqual(runtime["execution_token_budget"], 4)

    def test_rejects_linear_idle_threads(self) -> None:
        with self.assertRaisesRegex(
            check_session_executor.SessionExecutorCheckError,
            "idle sessions grew native threads",
        ):
            check_session_executor.parse_marker(
                "POSTGAMMA_SESSION_EXECUTOR connections=1000 "
                "idle_thread_growth=999 idle_fd_growth=4000 nofile_soft=16384 "
                "request_thread_growth=0 "
                "request_storm=1000 holder_progress=true saturated_waiters=4 "
                "pinning_cliff=true pinned_transactions=4 "
                "queued_at_cliff=true parallel_query=true executor_workers=4 "
                "request_threads=0 "
                "phase=closed\n"
            )

    def test_rejects_unexercised_pinning_cliff(self) -> None:
        with self.assertRaisesRegex(
            check_session_executor.SessionExecutorCheckError,
            "pinning cliff",
        ):
            check_session_executor.parse_runtime(
                valid_runtime().replace(
                    "pinned_sessions_peak=4", "pinned_sessions_peak=1"
                )
            )

    def test_rejects_execution_budget_oversubscription(self) -> None:
        with self.assertRaisesRegex(
            check_session_executor.SessionExecutorCheckError,
            "execution budget",
        ):
            check_session_executor.parse_runtime(
                valid_runtime().replace("execution_tokens_peak=4", "execution_tokens_peak=5")
            )


if __name__ == "__main__":
    unittest.main()
