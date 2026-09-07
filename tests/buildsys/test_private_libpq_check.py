from __future__ import annotations

import sys
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "buildsys"))

import check_private_libpq


class PrivateLibpqCheckTests(unittest.TestCase):
    def driver_report(self, iterations: int = 25) -> dict[str, object]:
        return {
            "status": 1,
            "warmup_status": 1,
            "warmup_checks": 14,
            "iterations": iterations,
            "completed_iterations": iterations,
            "checks": 14,
            "total_checks": iterations * 14,
            "queue_capacity": 64,
            "startup_packets": 1,
            "query_packets": 5,
            "copy_packets": 1,
            "copy_bytes_before_response": 128,
            "copy_bytes_received": 4096,
            "simultaneous_queue_saturation": 1,
            "secure_read_calls": 10,
            "secure_write_calls": 10,
            "socket_wait_calls": 10,
            "notice_count": 1,
            "cancel_dispatches": 1,
            "network_connect_calls": 0,
            "optional_security_calls": 0,
            "backend_pid": 4242,
            "connection_generation": 702001,
            "request_generation": 702002,
            "active_transports_after_close": 0,
            "endpoint_references_after_close": 0,
            "allocated_bytes_after_close": 0,
            "host_pid": 4242,
            "host_pid_unchanged": True,
            "resources_restored": True,
            "baseline_descriptors": 4,
            "baseline_threads": 1,
            "baseline_children": 0,
            "baseline_mappings": 24,
            "baseline_sysv_mappings": 0,
        }

    def test_accepts_complete_driver_report(self) -> None:
        report = self.driver_report()
        self.assertIs(
            check_private_libpq.validate_driver_report(report, 25), report
        )

    def test_rejects_missing_duplex_saturation(self) -> None:
        report = self.driver_report()
        report["simultaneous_queue_saturation"] = 0
        with self.assertRaisesRegex(
            check_private_libpq.PrivateLibpqCheckError, "incomplete"
        ):
            check_private_libpq.validate_driver_report(report, 25)

    def test_rejects_failed_resource_warmup(self) -> None:
        report = self.driver_report()
        report["warmup_status"] = 0
        with self.assertRaisesRegex(
            check_private_libpq.PrivateLibpqCheckError, "incomplete"
        ):
            check_private_libpq.validate_driver_report(report, 25)

    def test_trace_allows_only_thread_clones(self) -> None:
        driver = Path("/tmp/private-libpq-driver")
        trace = (
            '100 execve("/tmp/private-libpq-driver", [], []) = 0\n'
            "100 clone(child_stack=NULL, flags=CLONE_VM|CLONE_THREAD) = 101\n"
        )
        report = check_private_libpq.audit_trace(trace, driver, 1)
        self.assertEqual(report["network_syscalls"], 0)
        self.assertEqual(report["thread_clone_attempts"], 1)

    def test_trace_rejects_network_and_process_clones(self) -> None:
        driver = Path("/tmp/private-libpq-driver")
        with self.assertRaisesRegex(
            check_private_libpq.PrivateLibpqCheckError, "network syscall"
        ):
            check_private_libpq.audit_trace(
                'execve("/tmp/private-libpq-driver", [], []) = 0\n'
                "socket(AF_INET, SOCK_STREAM, 0) = 3\n",
                driver,
                1,
            )
        with self.assertRaisesRegex(
            check_private_libpq.PrivateLibpqCheckError, "non-thread clone"
        ):
            check_private_libpq.audit_trace(
                'execve("/tmp/private-libpq-driver", [], []) = 0\n'
                "clone(child_stack=NULL, flags=SIGCHLD) = 101\n",
                driver,
                1,
            )


if __name__ == "__main__":
    unittest.main()
