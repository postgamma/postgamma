"""Tests for the embedded host crash-recovery evidence validator."""

from __future__ import annotations

import signal
import sys
import unittest
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(PROJECT_ROOT / "buildsys"))

import check_embedded_crash  # noqa: E402


def writer_marker(host_pid: int = 31001) -> str:
    return (
        "POSTGAMMA_KERNEL_CRASH_READY generation=620001 "
        f"host_pid={host_pid} backend_pid=1000000007 "
        "committed_ack=true uncommitted_visible=true "
        "synchronous_commit=on\n"
    )


def recovery_marker() -> str:
    return (
        "POSTGAMMA_KERNEL_CRASH_RECOVERY generation=620001 "
        "host_pid=31002 backend_pid=1000000007 committed_rows=1 "
        "uncommitted_rows=0 recovery_complete=true transport=memory "
        "network_calls=0 active_transports_after_close=0 "
        "endpoint_references_after_close=0 phase=closed state=closed\n"
    )


def recovery_log() -> str:
    return "\n".join(check_embedded_crash.RECOVERY_LOG_MESSAGES)


class EmbeddedCrashEvidenceTests(unittest.TestCase):
    def test_accepts_sigkill_after_commit_barrier(self) -> None:
        report = check_embedded_crash.validate_killed_writer(
            -signal.SIGKILL, 31001, writer_marker()
        )
        self.assertEqual(report["termination"], "SIGKILL")

    def test_rejects_writer_that_exits_normally(self) -> None:
        with self.assertRaisesRegex(
            check_embedded_crash.CrashCheckError, "through SIGKILL"
        ):
            check_embedded_crash.validate_killed_writer(
                0, 31001, writer_marker()
            )

    def test_rejects_a_different_killed_host(self) -> None:
        with self.assertRaisesRegex(
            check_embedded_crash.CrashCheckError, "killed host"
        ):
            check_embedded_crash.validate_killed_writer(
                -signal.SIGKILL, 31002, writer_marker()
            )

    def test_accepts_recovered_committed_state(self) -> None:
        report = check_embedded_crash.validate_recovery(
            recovery_marker(), recovery_log()
        )
        self.assertEqual(report["status"], "pass")

    def test_rejects_surviving_uncommitted_state(self) -> None:
        with self.assertRaisesRegex(
            check_embedded_crash.CrashCheckError, "uncommitted_rows"
        ):
            check_embedded_crash.validate_recovery(
                recovery_marker().replace(
                    "uncommitted_rows=0", "uncommitted_rows=1"
                ),
                recovery_log(),
            )

    def test_requires_actual_recovery_logs(self) -> None:
        with self.assertRaisesRegex(
            check_embedded_crash.CrashCheckError, "WAL recovery evidence"
        ):
            check_embedded_crash.validate_recovery(
                recovery_marker(),
                "database system is ready to accept connections",
            )


if __name__ == "__main__":
    unittest.main()
