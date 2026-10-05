"""Tests for the session execution bounded session/executor evidence validator."""

from __future__ import annotations

import argparse
import sys
import unittest
from pathlib import Path
from unittest.mock import patch


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
    def test_trace_expression_covers_audited_calls_without_mask_churn(self) -> None:
        traced = set(
            check_session_executor.TRACE_EXPRESSION.removeprefix("trace=").split(",")
        )

        self.assertIn("process", traced)
        self.assertIn("network", traced)
        self.assertTrue(check_session_executor.FORBIDDEN_SIGNAL_CALLS <= traced)
        self.assertTrue(check_session_executor.FORBIDDEN_HOST_STATE_CALLS <= traced)
        self.assertNotIn("signal", traced)
        self.assertNotIn("rt_sigprocmask", traced)

    @patch("check_session_executor.pidfd_send_signal_available", return_value=False)
    @patch("check_session_executor.strace_recognizes_syscall")
    def test_omits_unrecognized_syscall_only_when_kernel_lacks_it(
        self, recognizes, _available
    ) -> None:
        recognizes.side_effect = lambda _strace, syscall: (
            syscall != "pidfd_send_signal"
        )

        expression, unavailable = check_session_executor.trace_expression_for_host(
            "strace"
        )

        self.assertNotIn("pidfd_send_signal", expression)
        self.assertEqual(unavailable, ("pidfd_send_signal",))
        self.assertTrue(
            (check_session_executor.AUDITED_SYSCALLS - {"pidfd_send_signal"})
            <= set(expression.removeprefix("trace=").split(","))
        )

    @patch("check_session_executor.pidfd_send_signal_available", return_value=True)
    @patch("check_session_executor.strace_recognizes_syscall")
    def test_rejects_old_strace_when_kernel_has_unrecognized_syscall(
        self, recognizes, _available
    ) -> None:
        recognizes.side_effect = lambda _strace, syscall: (
            syscall != "pidfd_send_signal"
        )

        with self.assertRaisesRegex(
            check_session_executor.SessionExecutorCheckError,
            "running kernel may implement it",
        ):
            check_session_executor.trace_expression_for_host("strace")

    @patch("check_session_executor.platform.machine", return_value="x86_64")
    @patch(
        "check_session_executor.ctypes.get_errno",
        return_value=check_session_executor.errno.ENOSYS,
    )
    @patch("check_session_executor.ctypes.CDLL")
    def test_pidfd_probe_uses_kernel_enosys(self, cdll, _errno, _machine) -> None:
        cdll.return_value.syscall.return_value = -1

        self.assertFalse(check_session_executor.pidfd_send_signal_available())

    def test_accepts_positive_finite_timeout(self) -> None:
        self.assertEqual(
            check_session_executor.positive_timeout_seconds("3600"), 3600.0
        )

    def test_accepts_positive_whole_request_timeout(self) -> None:
        self.assertEqual(check_session_executor.positive_integer_seconds("300"), 300)

    def test_rejects_invalid_request_timeout(self) -> None:
        for value in ("0", "-1", "1.5", "invalid"):
            with self.subTest(value=value), self.assertRaises(
                argparse.ArgumentTypeError
            ):
                check_session_executor.positive_integer_seconds(value)

    def test_rejects_invalid_timeout(self) -> None:
        for value in ("0", "-1", "nan", "inf", "invalid"):
            with self.subTest(value=value), self.assertRaises(
                argparse.ArgumentTypeError
            ):
                check_session_executor.positive_timeout_seconds(value)

    def test_accepts_bounded_product_evidence(self) -> None:
        marker = check_session_executor.parse_marker(
            "POSTGAMMA_SESSION_EXECUTOR connections=1000 "
            "idle_thread_growth=0 idle_fd_growth=4000 nofile_soft=16384 "
            "request_thread_growth=0 request_storm=1000 "
            "holder_progress=true waiter_requests=4 saturated_waiters=3 "
            "pinning_cliff=true "
            "pinned_transactions=4 queued_at_cliff=true close_retry=true "
            "parallel_query=true "
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
                "request_storm=1000 holder_progress=true waiter_requests=4 "
                "saturated_waiters=3 "
                "pinning_cliff=true pinned_transactions=4 "
                "queued_at_cliff=true close_retry=true parallel_query=true "
                "executor_workers=4 "
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

    def test_rejects_unobserved_lock_saturation(self) -> None:
        with self.assertRaisesRegex(
            check_session_executor.SessionExecutorCheckError,
            "saturated lock waiters",
        ):
            check_session_executor.parse_runtime(
                valid_runtime().replace(
                    "blocked_sessions_peak=3", "blocked_sessions_peak=1"
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
