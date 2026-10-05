"""Tests for the logical-management evidence gate."""

from __future__ import annotations

import argparse
import sys
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch


PROJECT_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(PROJECT_ROOT / "buildsys"))

import check_logical_management  # noqa: E402


class LogicalManagementTests(unittest.TestCase):
    def test_accepts_positive_whole_timeout(self) -> None:
        self.assertEqual(check_logical_management.positive_timeout_seconds("1800"), 1800)

    def test_rejects_invalid_timeout(self) -> None:
        for value in ("0", "-1", "1.5", "invalid"):
            with self.subTest(value=value), self.assertRaises(
                argparse.ArgumentTypeError
            ):
                check_logical_management.positive_timeout_seconds(value)

    @patch("strace_trace.subprocess.run")
    def test_strace_prefix_adapts_to_seccomp_option_support(self, run) -> None:
        run.return_value = SimpleNamespace(returncode=1)
        self.assertEqual(
            check_logical_management.strace_command_prefix("strace"), ["strace"]
        )
        run.return_value = SimpleNamespace(returncode=0)
        self.assertEqual(
            check_logical_management.strace_command_prefix("strace"),
            ["strace", "--seccomp-bpf"],
        )

    def test_trace_commands_use_the_detected_prefix(self) -> None:
        prefix = ["strace"]
        command = check_logical_management.traced_command(
            prefix, Path("trace"), Path("driver"), [Path("argument")]
        )
        filesystem = check_logical_management.filesystem_trace_command(
            prefix, Path("trace"), Path("driver"), [Path("argument")]
        )
        self.assertEqual(command[:2], ["strace", "-f"])
        self.assertEqual(filesystem[:2], ["strace", "-f"])
        self.assertIn("trace=file", filesystem)
        self.assertIn(check_logical_management.STREAM_SENTINEL, filesystem)

    def test_parses_complete_stream_marker(self) -> None:
        marker = (
            "POSTGAMMA_LOGICAL_STREAM pid=42 archive_bytes=68025 "
            "dump_calls=334 restore_calls=471 channel_capacity=4096 "
            "progress_quantum=1024 large_objects=1 metadata=true "
            "maintenance=true maintenance_operations=4 "
            "dependencies=true acl=true identity=true "
            "clean_restore=true invalid_flags=true callback_failure=true "
            "failure_retry=true cancel_retry=true "
            "cancel=true reentrant=true subprocesses=0 staging_files=0 "
            "stable_waitable=true callbacks_on_progress_thread=true "
            "concurrent_progress_busy=true concurrent_cancel=true "
            "running_free_cleanup=true "
            "phase=closed\n"
        )
        values = check_logical_management.parse_marker(
            marker,
            check_logical_management.STREAM_MARKER,
            check_logical_management.STREAM_VALUES,
        )
        self.assertEqual(values["archive_bytes"], 68025)
        self.assertEqual(values["dump_calls"], 334)

    def test_rejects_missing_retry_proof(self) -> None:
        marker = (
            "POSTGAMMA_LOGICAL_STREAM pid=42 archive_bytes=10 "
            "dump_calls=2 restore_calls=2 channel_capacity=4096 "
            "progress_quantum=1024 large_objects=1 metadata=true "
            "maintenance=true maintenance_operations=4 "
            "dependencies=true acl=true identity=true "
            "clean_restore=true invalid_flags=true callback_failure=true "
            "cancel_retry=true cancel=true reentrant=true "
            "stable_waitable=true callbacks_on_progress_thread=true "
            "concurrent_progress_busy=true concurrent_cancel=true "
            "running_free_cleanup=true "
            "subprocesses=0 staging_files=0 phase=closed\n"
        )
        with self.assertRaisesRegex(
            check_logical_management.LogicalManagementCheckError,
            "failure_retry",
        ):
            check_logical_management.parse_marker(
                marker,
                check_logical_management.STREAM_MARKER,
                check_logical_management.STREAM_VALUES,
            )

    def test_rejects_frontend_diagnostic_leak(self) -> None:
        with self.assertRaisesRegex(
            check_logical_management.LogicalManagementCheckError,
            "leaked",
        ):
            check_logical_management.require_diagnostics_isolated(
                "postgamma-pg-restore: error: bad archive\n"
            )


if __name__ == "__main__":
    unittest.main()
