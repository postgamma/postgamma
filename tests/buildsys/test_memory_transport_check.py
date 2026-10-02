"""Tests for the bounded memory transport evidence validator."""

from __future__ import annotations

import sys
import unittest
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(PROJECT_ROOT / "buildsys"))

import check_memory_transport  # noqa: E402


def valid_report() -> dict[str, object]:
    return {
        "schema_version": 1,
        "kind": check_memory_transport.RUN_KIND,
        "status": "pass",
        "scenarios": 7,
        "capacities": [1, 7, 64],
        "repeated_lifecycles": 600,
        "duplex_progress": {
            "capacity": 7,
            "copy_bytes": 4096,
            "control_bytes": 257,
            "both_queues_saturated": True,
            "wait_calls": 100,
            "wait_wakeups": 80,
            "timeouts": 0,
        },
        "global_baseline": {
            "active_transports": 0,
            "endpoint_references": 0,
            "allocated_bytes": 0,
        },
        "global_final": {
            "active_transports": 0,
            "endpoint_references": 0,
            "allocated_bytes": 0,
        },
        "process_baseline": {"descriptors": 4, "threads": 1},
        "process_final": {"descriptors": 4, "threads": 1},
    }


class MemoryTransportCheckTests(unittest.TestCase):
    def test_accepts_complete_progress_and_resource_evidence(self) -> None:
        report = valid_report()
        self.assertIs(check_memory_transport.validate_run_report(report), report)

    def test_rejects_a_duplex_timeout(self) -> None:
        report = valid_report()
        report["duplex_progress"]["timeouts"] = 1
        with self.assertRaisesRegex(
            check_memory_transport.MemoryTransportCheckError,
            "duplex saturation",
        ):
            check_memory_transport.validate_run_report(report)

    def test_rejects_a_transport_resource_leak(self) -> None:
        report = valid_report()
        report["global_final"]["allocated_bytes"] = 128
        with self.assertRaisesRegex(
            check_memory_transport.MemoryTransportCheckError,
            "return to zero",
        ):
            check_memory_transport.validate_run_report(report)


if __name__ == "__main__":
    unittest.main()
