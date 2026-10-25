"""Tests for the bounded memory transport evidence validator."""

from __future__ import annotations

import json
import os
import re
import shlex
import signal
import subprocess
import sys
import tempfile
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
    def test_process_baseline_waits_for_exit_but_rejects_leaks(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            binary = Path(temporary) / "memory-transport-baseline"
            report = Path(temporary) / "report.json"
            command = [
                *shlex.split(os.environ.get("CC", "cc")),
                "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", "-Wpedantic",
                "-pthread", "-Iembedded-c/include",
                "embedded-c/src/memory_transport.c",
                "embedded-c/tests/fixtures/memory_transport_baseline.c",
                "-o", str(binary),
            ]
            compiled = subprocess.run(
                command, cwd=PROJECT_ROOT, capture_output=True, text=True, timeout=60,
            )
            self.assertEqual(
                compiled.returncode, 0,
                f"{shlex.join(command)}\n{compiled.stdout}\n{compiled.stderr}",
            )
            # Expected aborts must not leave core dumps in the source checkout.
            launcher = (
                "import os, resource, sys; "
                "resource.setrlimit(resource.RLIMIT_CORE, (0, 0)); "
                "os.execv(sys.argv[1], sys.argv[1:])"
            )
            for scenario in (
                "transient-thread", "helper-thread", "stuck-warm-thread",
                "persistent-thread", "leaked-descriptor",
            ):
                with self.subTest(scenario=scenario):
                    result = subprocess.run(
                        [sys.executable, "-c", launcher, str(binary), scenario,
                         "--report", str(report)],
                        capture_output=True, text=True, timeout=15,
                    )
                    if scenario in ("transient-thread", "helper-thread"):
                        self.assertEqual(result.returncode, 0, result.stderr)
                        document = json.loads(report.read_text(encoding="utf-8"))
                        self.assertEqual(
                            document["process_baseline"], document["process_final"],
                        )
                        if scenario == "transient-thread":
                            check_memory_transport.validate_run_report(document)
                            self.assertEqual(document["process_baseline"]["threads"], 1)
                        else:
                            # A persistent sanitizer helper belongs in its baseline.
                            self.assertEqual(document["process_baseline"]["threads"], 2)
                    elif scenario == "stuck-warm-thread":
                        self.assertEqual(result.returncode, -signal.SIGABRT, result.stderr)
                        self.assertRegex(
                            result.stderr, r"warm-up thread \d+ did not exit before baseline",
                        )
                    else:
                        self.assertEqual(result.returncode, -signal.SIGABRT, result.stderr)
                        expected_threads = 2 if scenario == "persistent-thread" else 1
                        counts = re.search(
                            rf"expected descriptors=(\d+) threads=1; "
                            rf"observed descriptors=(\d+) threads={expected_threads}; "
                            r"stable samples=0/3", result.stderr,
                        )
                        self.assertIsNotNone(counts, result.stderr)
                        self.assertEqual(
                            int(counts[2]),
                            int(counts[1]) + (scenario == "leaked-descriptor"),
                        )

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
