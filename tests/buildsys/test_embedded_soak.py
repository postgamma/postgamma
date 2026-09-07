from __future__ import annotations

import sys
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
BUILDSYS = ROOT / "buildsys"
if str(BUILDSYS) not in sys.path:
    sys.path.insert(0, str(BUILDSYS))

import check_embedded_soak


def valid_marker(iterations: int = 1000, seed: int = 20260825) -> str:
    schedule = check_embedded_soak.expected_schedule(iterations)
    return (
        f"POSTGAMMA_KERNEL_SOAK seed={seed} iterations={iterations} "
        f"lifecycle_cycles={iterations} query_cycles={iterations} "
        f"fault_cycles={iterations} "
        f"data_a_cycles={schedule['data_a_cycles']} "
        f"data_b_cycles={schedule['data_b_cycles']} "
        f"fast_shutdowns={schedule['fast_shutdowns']} "
        f"immediate_shutdowns={schedule['immediate_shutdowns']} "
        f"recovery_queries={schedule['recovery_queries']} "
        "fault_points=10 fault_coverage=0x3ff fault_callbacks=5500 "
        f"fault_injections={iterations} failure_records={iterations} "
        "cleanup_failures=0 network_calls=0 "
        f"secure_reads={iterations * 2} secure_writes={iterations * 2} "
        f"process_checks={iterations * 2} mapping_checks={iterations * 2} "
        f"memory_checks={iterations * 2} active_transports=0 "
        "endpoint_references=0 active_locks=0 active_instances=0 "
        "active_memory_contexts=0 resources_restored=true "
        "mapping_baseline=94 mapping_peak=96 mapping_final=96 "
        "mapping_delta_budget=32 mapping_first_half_peak=96 "
        "mapping_second_half_peak=96 mapping_steady_drift_budget=4 "
        "virtual_kib_baseline=100000 "
        "virtual_kib_peak=120000 virtual_kib_final=120000 "
        "virtual_kib_delta_budget=262144 virtual_kib_first_half_peak=120000 "
        "virtual_kib_second_half_peak=120000 "
        "virtual_kib_steady_drift_budget=32768 resident_kib_baseline=10000 "
        "resident_kib_peak=12000 resident_kib_final=12000 "
        "resident_kib_delta_budget=131072 resident_kib_first_half_peak=12000 "
        "resident_kib_second_half_peak=12000 "
        "resident_kib_steady_drift_budget=16384 "
        "allocator_metrics_supported=true allocator_live_baseline=2000000 "
        "allocator_live_peak=2100000 allocator_live_final=2050000 "
        "allocator_live_delta_budget=8388608 "
        "allocator_live_first_half_peak=2100000 "
        "allocator_live_second_half_peak=2100000 "
        "allocator_live_steady_drift_budget=1048576 mappings_bounded=true "
        "allocator_live_bounded=true "
        "steady_state_bounded=true "
        "process_globals_restored=true "
        "pid_files_absent=true duration_ms=120000 host_pid=1234\n"
    )


class EmbeddedSoakEvidenceTests(unittest.TestCase):
    def test_accepts_complete_marker(self) -> None:
        values = check_embedded_soak.parse_marker(
            valid_marker(), 1000, 20260825
        )
        self.assertEqual(values["data_a_cycles"], 667)
        self.assertEqual(values["immediate_shutdowns"], 333)
        self.assertEqual(values["fault_coverage"], 0x3FF)

    def test_rejects_short_run(self) -> None:
        with self.assertRaisesRegex(
            check_embedded_soak.SoakCheckError, "between 1000"
        ):
            check_embedded_soak.validate_iterations(999)

    def test_rejects_missing_fault_record(self) -> None:
        marker = valid_marker().replace(
            "failure_records=1000", "failure_records=999"
        )
        with self.assertRaisesRegex(
            check_embedded_soak.SoakCheckError, "failure_records"
        ):
            check_embedded_soak.parse_marker(marker, 1000, 20260825)

    def test_rejects_mapping_drift(self) -> None:
        marker = valid_marker().replace(
            "mapping_peak=96", "mapping_peak=127"
        )
        with self.assertRaisesRegex(
            check_embedded_soak.SoakCheckError, "mapping exceeded"
        ):
            check_embedded_soak.parse_marker(marker, 1000, 20260825)

    def test_requires_recovery_messages(self) -> None:
        stderr = "\n".join(
            [
                "database system was interrupted",
                "automatic recovery in progress",
                "received immediate shutdown request",
                "database system is ready to accept connections",
            ]
            * 334
        )
        with self.assertRaisesRegex(
            check_embedded_soak.SoakCheckError, "READY"
        ):
            check_embedded_soak.validate_recovery_logs(stderr, 1000)

    def test_rejects_steady_state_growth(self) -> None:
        marker = valid_marker().replace(
            "resident_kib_peak=12000",
            "resident_kib_peak=30000",
        ).replace(
            "resident_kib_second_half_peak=12000",
            "resident_kib_second_half_peak=30000",
        )
        with self.assertRaisesRegex(
            check_embedded_soak.SoakCheckError, "steady-state drift"
        ):
            check_embedded_soak.parse_marker(marker, 1000, 20260825)

    def test_rejects_allocator_live_growth(self) -> None:
        marker = valid_marker().replace(
            "allocator_live_peak=2100000",
            "allocator_live_peak=12000000",
        ).replace(
            "allocator_live_second_half_peak=2100000",
            "allocator_live_second_half_peak=12000000",
        )
        with self.assertRaisesRegex(
            check_embedded_soak.SoakCheckError, "allocator live"
        ):
            check_embedded_soak.parse_marker(marker, 1000, 20260825)


if __name__ == "__main__":
    unittest.main()
