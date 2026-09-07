"""Tests for the embedded shutdown/recovery lifecycle validator."""

from __future__ import annotations

import errno
import sys
import unittest
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(PROJECT_ROOT / "buildsys"))

import check_embedded_lifecycle  # noqa: E402


def valid_marker() -> str:
    return (
        "POSTGAMMA_KERNEL_LIFECYCLE generation=4 cycles=4 "
        "smart_shutdown=true fast_shutdown=true immediate_shutdown=true "
        "recovery_reopen=true phase=closed state=closed "
        "resources_restored=true mappings_stable=true "
        "cold_mapping_growth=7 steady_mapping_growth=0 "
        "cwd_restored=true locale_restored=true "
        "environment_restored=true signals_restored=true "
        "pid_file_absent=true role_process_launches=0\n"
    )


def valid_footprint() -> str:
    return (
        "POSTGAMMA_KERNEL_FOOTPRINT generation=4 profile=embedded-default "
        "cold_open_to_ready_ms=229 warm_open_to_ready_ms=193 "
        "immediate_open_to_ready_ms=205 recovery_open_to_ready_ms=575 "
        "maximum_open_to_ready_ms=575 peak_rss_delta_kib=24320 "
        "peak_virtual_delta_kib=646716 peak_thread_delta=7 "
        "peak_fd_delta=23\n"
    )


def valid_supervisor_stack() -> str:
    return (
        "POSTGAMMA_KERNEL_SUPERVISOR_STACK generation=4 cycles_validated=4 "
        "class=supervisor configured_stack_bytes=4194304 "
        "configured_guard_bytes=65536 native_stack_bytes=4194304 "
        "native_guard_bytes=65536 usable_stack_bytes=4128768 "
        "actual_bounds=true\n"
    )


def valid_fault_matrix() -> str:
    return (
        "POSTGAMMA_KERNEL_FAULT_MATRIX generation=1010 cycles=10 "
        "fault_points=10 callbacks=55 injections=10 cleanup_failures=0 "
        "failure_records=10 host_fail_stop_records=10 "
        "process_restart_required=0 supervisor_threads_exited=10 "
        "supervisor_threads_joined=10 resources_restored=true "
        f"mappings_stable=true fault_status={errno.ECANCELED}\n"
    )


def valid_kernel_telemetry() -> str:
    return (
        "POSTGAMMA_KERNEL_TELEMETRY generation=1010 "
        "instances_entered=14 instances_closed=4 instances_failed=10 "
        "cleanup_failures=0 active_instances=0 active_memory_contexts=0\n"
    )


def backend_stack_line(
    generation: int,
    class_name: str,
    observations: int,
) -> str:
    if observations == 0:
        values = {name: 0 for name in check_embedded_lifecycle.BACKEND_STACK_FIELDS}
    else:
        values = {
            "observations": observations,
            "observation_failures": 0,
            "depth_validations": observations,
            "depth_validation_failures": 0,
            "accounting_failures": 0,
            "active_reservation_bytes": 0,
            "peak_reservation_bytes": 25165824,
            "configured_stack_min": 4194304,
            "configured_stack_max": 4194304,
            "configured_guard_min": 65536,
            "native_stack_min": 4194304,
            "native_guard_min": 65536,
            "usable_stack_min": 4128768,
            "depth_limit_min": 3342336,
            "configured_depth_max": 2097152,
        }
    fields = " ".join(f"{name}={values[name]}" for name in values)
    return (
        f"LOG: POSTGAMMA_STACK generation={generation} "
        f"class={class_name} {fields}"
    )


def valid_backend_stack_output() -> str:
    lines = []
    for generation in range(1, 5):
        lines.append(backend_stack_line(generation, "dedicated", 7))
        lines.append(backend_stack_line(generation, "client", 0))
        lines.append(backend_stack_line(generation, "parallel", 0))
    return "\n".join(lines) + "\n"


def runtime_line(host_pid: int = 1234, dedicated_threads: int = 7) -> str:
    return (
        "LOG: POSTGAMMA_RUNTIME backend_model=thread "
        f"host_pid={host_pid} provider=pooled threads_started=true "
        "role_process_launches=0 forbidden_process_launch_attempts=0 "
        "unsupported_role_requests=0 client_threads_started=4 "
        "parallel_threads_started=0 "
        f"dedicated_threads_started={dedicated_threads} "
        f"role_completions={dedicated_threads} role_threads_active=0 "
        "pooled_worker_threads=4 execution_tokens_active=0 "
        "execution_token_budget=4 execution_token_rejections=0"
    )


def valid_runtime_output() -> str:
    return "\n".join(runtime_line() for _ in range(4)) + "\n"


def valid_budget() -> dict[str, object]:
    return {
        "schema_version": 1,
        "baseline_id": "test-pg19-v1",
        "postgresql_major": 19,
        "profile": "embedded-default",
        "measurement_source": "linux-procfs",
        "budgets": {
            "max_cold_open_to_ready_ms": 5000,
            "max_warm_open_to_ready_ms": 3000,
            "max_immediate_open_to_ready_ms": 3000,
            "max_recovery_open_to_ready_ms": 8000,
            "max_any_open_to_ready_ms": 8000,
            "max_peak_rss_delta_kib": 98304,
            "max_peak_virtual_delta_kib": 1048576,
            "max_peak_thread_delta": 8,
            "max_peak_fd_delta": 32,
            "expected_dedicated_threads_per_cycle": 7,
            "min_supervisor_stack_bytes": 4194304,
            "max_supervisor_stack_bytes": 4194304,
            "min_dedicated_stack_bytes": 4194304,
            "max_dedicated_stack_bytes": 4194304,
            "min_client_stack_bytes": 8388608,
            "max_client_stack_bytes": 8388608,
            "min_stack_guard_bytes": 65536,
            "max_dedicated_peak_reservation_bytes": 33554432,
            "max_client_peak_reservation_bytes": 16777216,
        },
    }


class EmbeddedLifecycleEvidenceTests(unittest.TestCase):
    def test_accepts_complete_shutdown_matrix(self) -> None:
        values = check_embedded_lifecycle.parse_marker(valid_marker())
        self.assertEqual(values["cycles"], "4")
        self.assertEqual(values["recovery_reopen"], "true")

    def test_rejects_missing_immediate_shutdown(self) -> None:
        marker = valid_marker().replace(
            "immediate_shutdown=true", "immediate_shutdown=false"
        )
        with self.assertRaisesRegex(
            check_embedded_lifecycle.LifecycleCheckError,
            "immediate_shutdown",
        ):
            check_embedded_lifecycle.parse_marker(marker)

    def test_accepts_complete_fault_matrix(self) -> None:
        values = check_embedded_lifecycle.parse_fault_matrix_marker(
            valid_fault_matrix()
        )
        self.assertEqual(values["fault_points"], 10)
        self.assertEqual(values["callbacks"], 55)

    def test_rejects_fault_cleanup_failure(self) -> None:
        marker = valid_fault_matrix().replace(
            "cleanup_failures=0", "cleanup_failures=1"
        )
        with self.assertRaisesRegex(
            check_embedded_lifecycle.LifecycleCheckError,
            "cleanup_failures",
        ):
            check_embedded_lifecycle.parse_fault_matrix_marker(marker)

    def test_rejects_missing_failure_record(self) -> None:
        marker = valid_fault_matrix().replace(
            "failure_records=10", "failure_records=9"
        )
        with self.assertRaisesRegex(
            check_embedded_lifecycle.LifecycleCheckError,
            "failure_records",
        ):
            check_embedded_lifecycle.parse_fault_matrix_marker(marker)

    def test_accepts_clean_kernel_telemetry(self) -> None:
        values = check_embedded_lifecycle.parse_kernel_telemetry_marker(
            valid_kernel_telemetry()
        )
        self.assertEqual(values["instances_entered"], 14)
        self.assertEqual(values["active_memory_contexts"], 0)

    def test_rejects_active_memory_context(self) -> None:
        marker = valid_kernel_telemetry().replace(
            "active_memory_contexts=0", "active_memory_contexts=1"
        )
        with self.assertRaisesRegex(
            check_embedded_lifecycle.LifecycleCheckError,
            "active_memory_contexts",
        ):
            check_embedded_lifecycle.parse_kernel_telemetry_marker(marker)

    def test_accepts_complete_recovery_log(self) -> None:
        report = check_embedded_lifecycle.validate_lifecycle_logs(
            "\n".join(check_embedded_lifecycle.REQUIRED_LOG_MESSAGES)
            + "\nredo is not required\n"
        )
        self.assertEqual(report["status"], "pass")
        self.assertEqual(report["redo_conclusion"], "redo is not required")

    def test_rejects_missing_recovery_log(self) -> None:
        with self.assertRaisesRegex(
            check_embedded_lifecycle.LifecycleCheckError,
            "complete recovery evidence",
        ):
            check_embedded_lifecycle.validate_lifecycle_logs(
                "received immediate shutdown request\n"
                "database system is ready to accept connections\n"
            )

    def test_rejects_recovery_without_redo_conclusion(self) -> None:
        with self.assertRaisesRegex(
            check_embedded_lifecycle.LifecycleCheckError,
            "WAL redo conclusion",
        ):
            check_embedded_lifecycle.validate_lifecycle_logs(
                "\n".join(check_embedded_lifecycle.REQUIRED_LOG_MESSAGES)
            )

    def test_accepts_footprint_within_versioned_budget(self) -> None:
        measurements = check_embedded_lifecycle.parse_footprint_marker(
            valid_footprint()
        )
        report = check_embedded_lifecycle.validate_resource_budget(
            valid_budget(), measurements
        )
        self.assertEqual(report["status"], "pass")
        self.assertEqual(report["baseline_id"], "test-pg19-v1")

    def test_rejects_latency_above_budget(self) -> None:
        measurements = check_embedded_lifecycle.parse_footprint_marker(
            valid_footprint().replace(
                "warm_open_to_ready_ms=193", "warm_open_to_ready_ms=3001"
            )
        )
        with self.assertRaisesRegex(
            check_embedded_lifecycle.LifecycleCheckError,
            "warm_open_to_ready_ms=3001",
        ):
            check_embedded_lifecycle.validate_resource_budget(
                valid_budget(), measurements
            )

    def test_accepts_transient_thread_footprint_below_role_count(self) -> None:
        measurements = check_embedded_lifecycle.parse_footprint_marker(
            valid_footprint().replace(
                "peak_thread_delta=7", "peak_thread_delta=1"
            )
        )
        report = check_embedded_lifecycle.validate_resource_budget(
            valid_budget(), measurements
        )
        self.assertEqual(report["status"], "pass")

    def test_accepts_exact_runtime_thread_contract(self) -> None:
        report = check_embedded_lifecycle.validate_runtime_thread_contract(
            valid_runtime_output(), 4, 7
        )
        self.assertEqual(report["status"], "pass")
        self.assertEqual(len(report["records"]), 4)

    def test_rejects_missing_runtime_cycle(self) -> None:
        with self.assertRaisesRegex(
            check_embedded_lifecycle.LifecycleCheckError,
            "expected 4 POSTGAMMA_RUNTIME lines",
        ):
            check_embedded_lifecycle.validate_runtime_thread_contract(
                "\n".join(runtime_line() for _ in range(3)), 4, 7
            )

    def test_rejects_incomplete_runtime_worker_pool(self) -> None:
        output = valid_runtime_output().replace(
            "pooled_worker_threads=4", "pooled_worker_threads=3", 1
        )
        with self.assertRaisesRegex(
            check_embedded_lifecycle.LifecycleCheckError,
            "pooled_worker_threads=3, expected=4",
        ):
            check_embedded_lifecycle.validate_runtime_thread_contract(
                output, 4, 7
            )

    def test_rejects_missing_dedicated_runtime_role(self) -> None:
        output = valid_runtime_output().replace(
            "dedicated_threads_started=7", "dedicated_threads_started=6", 1
        )
        with self.assertRaisesRegex(
            check_embedded_lifecycle.LifecycleCheckError,
            "dedicated_threads_started=6, expected=7",
        ):
            check_embedded_lifecycle.validate_runtime_thread_contract(
                output, 4, 7
            )

    def test_accepts_actual_supervisor_stack_within_budget(self) -> None:
        stack = check_embedded_lifecycle.parse_supervisor_stack_marker(
            valid_supervisor_stack()
        )
        report = check_embedded_lifecycle.validate_supervisor_stack_budget(
            valid_budget(), stack
        )
        self.assertEqual(report["status"], "pass")

    def test_rejects_inconsistent_supervisor_usable_stack(self) -> None:
        stack = check_embedded_lifecycle.parse_supervisor_stack_marker(
            valid_supervisor_stack().replace(
                "usable_stack_bytes=4128768", "usable_stack_bytes=4128767"
            )
        )
        with self.assertRaisesRegex(
            check_embedded_lifecycle.LifecycleCheckError,
            "usable stack is inconsistent",
        ):
            check_embedded_lifecycle.validate_supervisor_stack_budget(
                valid_budget(), stack
            )

    def test_accepts_complete_backend_stack_contract(self) -> None:
        records = check_embedded_lifecycle.parse_backend_stack_telemetry(
            valid_backend_stack_output(), tuple(range(1, 5))
        )
        report = check_embedded_lifecycle.validate_backend_stack_budget(
            valid_budget(),
            records,
            {"dedicated": 7, "client": 0, "parallel": 0},
        )
        self.assertEqual(report["status"], "pass")
        self.assertEqual(len(report["records"]), 12)

    def test_rejects_missing_backend_stack_record(self) -> None:
        output = valid_backend_stack_output().replace(
            backend_stack_line(4, "parallel", 0) + "\n", ""
        )
        with self.assertRaisesRegex(
            check_embedded_lifecycle.LifecycleCheckError,
            "records do not match expected set",
        ):
            check_embedded_lifecycle.parse_backend_stack_telemetry(
                output, tuple(range(1, 5))
            )

    def test_rejects_incomplete_stack_depth_validation(self) -> None:
        output = valid_backend_stack_output().replace(
            "generation=2 class=dedicated observations=7 "
            "observation_failures=0 depth_validations=7",
            "generation=2 class=dedicated observations=7 "
            "observation_failures=0 depth_validations=6",
        )
        records = check_embedded_lifecycle.parse_backend_stack_telemetry(
            output, tuple(range(1, 5))
        )
        with self.assertRaisesRegex(
            check_embedded_lifecycle.LifecycleCheckError,
            "validation is incomplete",
        ):
            check_embedded_lifecycle.validate_backend_stack_budget(
                valid_budget(),
                records,
                {"dedicated": 7, "client": 0, "parallel": 0},
            )

    def test_rejects_stack_observation_failure(self) -> None:
        output = valid_backend_stack_output().replace(
            "observation_failures=0", "observation_failures=1", 1
        )
        records = check_embedded_lifecycle.parse_backend_stack_telemetry(
            output, tuple(range(1, 5))
        )
        with self.assertRaisesRegex(
            check_embedded_lifecycle.LifecycleCheckError,
            "stack observation failed",
        ):
            check_embedded_lifecycle.validate_backend_stack_budget(
                valid_budget(),
                records,
                {"dedicated": 7, "client": 0, "parallel": 0},
            )

    def test_rejects_stack_accounting_failure(self) -> None:
        output = valid_backend_stack_output().replace(
            "accounting_failures=0", "accounting_failures=1", 1
        )
        records = check_embedded_lifecycle.parse_backend_stack_telemetry(
            output, tuple(range(1, 5))
        )
        with self.assertRaisesRegex(
            check_embedded_lifecycle.LifecycleCheckError,
            "stack accounting failed",
        ):
            check_embedded_lifecycle.validate_backend_stack_budget(
                valid_budget(),
                records,
                {"dedicated": 7, "client": 0, "parallel": 0},
            )

    def test_rejects_one_oversized_configured_stack(self) -> None:
        output = valid_backend_stack_output().replace(
            "configured_stack_max=4194304",
            "configured_stack_max=8388608",
            1,
        )
        records = check_embedded_lifecycle.parse_backend_stack_telemetry(
            output, tuple(range(1, 5))
        )
        with self.assertRaisesRegex(
            check_embedded_lifecycle.LifecycleCheckError,
            "configured stack is outside budget",
        ):
            check_embedded_lifecycle.validate_backend_stack_budget(
                valid_budget(),
                records,
                {"dedicated": 7, "client": 0, "parallel": 0},
            )

    def test_rejects_max_stack_depth_above_actual_limit(self) -> None:
        output = valid_backend_stack_output().replace(
            "depth_limit_min=3342336 configured_depth_max=2097152",
            "depth_limit_min=2097151 configured_depth_max=2097152",
            1,
        )
        records = check_embedded_lifecycle.parse_backend_stack_telemetry(
            output, tuple(range(1, 5))
        )
        with self.assertRaisesRegex(
            check_embedded_lifecycle.LifecycleCheckError,
            "max_stack_depth exceeds actual stack limit",
        ):
            check_embedded_lifecycle.validate_backend_stack_budget(
                valid_budget(),
                records,
                {"dedicated": 7, "client": 0, "parallel": 0},
            )

    def test_rejects_active_stack_reservation_after_close(self) -> None:
        output = valid_backend_stack_output().replace(
            "active_reservation_bytes=0", "active_reservation_bytes=4194304", 1
        )
        records = check_embedded_lifecycle.parse_backend_stack_telemetry(
            output, tuple(range(1, 5))
        )
        with self.assertRaisesRegex(
            check_embedded_lifecycle.LifecycleCheckError,
            "reservation remained active",
        ):
            check_embedded_lifecycle.validate_backend_stack_budget(
                valid_budget(),
                records,
                {"dedicated": 7, "client": 0, "parallel": 0},
            )


if __name__ == "__main__":
    unittest.main()
