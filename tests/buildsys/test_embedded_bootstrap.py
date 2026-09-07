"""Tests for the repeated in-process bootstrap evidence gate."""

from __future__ import annotations

import sys
import unittest
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(PROJECT_ROOT / "buildsys"))

import check_embedded_bootstrap  # noqa: E402


def valid_report() -> dict[str, object]:
    return {
        "schema_version": 1,
        "kind": check_embedded_bootstrap.RUN_KIND,
        "status": "pass",
        "library_loads": 1,
        "bootstrap_calls": 2,
        "distinct_system_identifiers": True,
        "baseline": {
            "host_pid": 123,
            "descriptors": 4,
            "threads": 1,
            "children": 0,
            "sysv_mappings": 0,
        },
        "runs": [
            {
                "generation": generation,
                "status": 1,
                "checks": 5,
                "exit_code": 0,
                "resources_remaining": 0,
                "system_identifier": 100 + generation,
                "cwd_restored": True,
                "stdin_restored": True,
                "host_pid_unchanged": True,
                "signals_restored": True,
                "pid_file_absent": True,
                "resources_restored": True,
                "driver_error": 0,
            }
            for generation in (1, 2)
        ],
    }


class EmbeddedBootstrapTests(unittest.TestCase):
    def test_accepts_exact_success_report(self) -> None:
        report = valid_report()
        self.assertIs(check_embedded_bootstrap.validate_driver_report(report), report)

    def test_rejects_a_resource_leak(self) -> None:
        report = valid_report()
        report["runs"][1]["resources_restored"] = False
        with self.assertRaisesRegex(
            check_embedded_bootstrap.BootstrapCheckError, "failed its contract"
        ):
            check_embedded_bootstrap.validate_driver_report(report)

    def test_trace_allows_only_initial_exec_and_sysv_lifecycle(self) -> None:
        driver = Path("/tmp/bootstrap-driver")
        trace = (
            'execve("/tmp/bootstrap-driver", ["/tmp/bootstrap-driver"], 0) = 0\n'
            "shmat(1, NULL, 0) = 0x1000\n"
            "shmdt(0x1000) = 0\n"
            "shmctl(1, IPC_RMID, NULL) = 0\n"
            "shmat(2, NULL, 0) = 0x2000\n"
            "shmdt(0x2000) = 0\n"
            "shmctl(2, IPC_RMID, NULL) = 0\n"
        )
        report = check_embedded_bootstrap.audit_trace(trace, driver)
        self.assertEqual(report["process_creation_calls"], 0)
        self.assertEqual(report["sysv_attach_calls"], 2)

    def test_trace_rejects_incomplete_sysv_teardown(self) -> None:
        trace = (
            'execve("/tmp/bootstrap-driver", ["/tmp/bootstrap-driver"], 0) = 0\n'
            "shmat(1, NULL, 0) = 0x1000\n"
            "shmdt(0x1000) = 0\n"
            "shmctl(1, IPC_RMID, NULL) = 0\n"
        )
        with self.assertRaisesRegex(
            check_embedded_bootstrap.BootstrapCheckError, "each bootstrap"
        ):
            check_embedded_bootstrap.audit_trace(
                trace, Path("/tmp/bootstrap-driver")
            )

    def test_trace_rejects_process_creation(self) -> None:
        trace = (
            'execve("/tmp/bootstrap-driver", ["/tmp/bootstrap-driver"], 0) = 0\n'
            "clone(child_stack=NULL, flags=SIGCHLD) = 123\n"
        )
        with self.assertRaisesRegex(
            check_embedded_bootstrap.BootstrapCheckError, "process-creation"
        ):
            check_embedded_bootstrap.audit_trace(
                trace, Path("/tmp/bootstrap-driver")
            )


if __name__ == "__main__":
    unittest.main()
