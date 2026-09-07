from __future__ import annotations

import sys
import unittest
from pathlib import Path
from unittest.mock import patch


PROJECT_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(PROJECT_ROOT / "buildsys"))

import check_threaded_runtime  # noqa: E402
import check_thread_isolation  # noqa: E402


def valid_telemetry() -> str:
    return (
        "2026-01-01 LOG: POSTGAMMA_RUNTIME backend_model=thread host_pid=123 "
        "threads_started=true role_process_launches=0 "
        "forbidden_process_launch_attempts=0 unsupported_role_requests=0 "
        "client_threads_started=8 client_threads_peak=4 "
        "parallel_threads_started=2 dedicated_threads_started=3 "
        "role_completions=13 role_threads_active=0"
    )


class ThreadedRuntimeTelemetryTests(unittest.TestCase):
    def test_accepts_complete_thread_only_telemetry(self) -> None:
        values = check_threaded_runtime.parse_telemetry(valid_telemetry())
        numbers = check_threaded_runtime.validate_telemetry(values)
        self.assertEqual(numbers["client_threads_peak"], 4)
        self.assertEqual(numbers["role_process_launches"], 0)

    def test_rejects_a_role_process_launch(self) -> None:
        values = check_threaded_runtime.parse_telemetry(
            valid_telemetry().replace("role_process_launches=0", "role_process_launches=1")
        )
        with self.assertRaisesRegex(
            check_threaded_runtime.ThreadedRuntimeError,
            "role_process_launches is not zero",
        ):
            check_threaded_runtime.validate_telemetry(values)

    def test_rejects_incomplete_role_lifecycle(self) -> None:
        values = check_threaded_runtime.parse_telemetry(
            valid_telemetry().replace("role_completions=13", "role_completions=12")
        )
        with self.assertRaisesRegex(
            check_threaded_runtime.ThreadedRuntimeError,
            "completion count",
        ):
            check_threaded_runtime.validate_telemetry(values)

    def test_rejects_missing_summary(self) -> None:
        with self.assertRaisesRegex(
            check_threaded_runtime.ThreadedRuntimeError,
            "telemetry is absent",
        ):
            check_threaded_runtime.parse_telemetry("ordinary PostgreSQL log")


class ThreadIsolationWorkloadTests(unittest.TestCase):
    def test_parallel_guc_probe_requires_worker_and_preserved_provenance(self) -> None:
        output = "128|command line\nWorkers Launched: 1\n128|command line"
        result = check_threaded_runtime.QueryResult(0, 0.01, output, "")
        with patch.object(check_threaded_runtime, "run_query", return_value=result) as run:
            check_threaded_runtime.validate_parallel_guc_restore(
                ["psql"], {}, Path("/tmp/probe's/guc_restore_probe.so")
            )
        sql = run.call_args.args[1]
        self.assertIn("SET debug_parallel_query=on", sql)
        self.assertIn("SET default_text_search_config='pg_catalog.simple'", sql)
        self.assertIn("LOAD '/tmp/probe''s/guc_restore_probe.so'", sql)

    def test_parallel_guc_probe_rejects_restore_errors_serial_plans_and_state_loss(self) -> None:
        cases = (
            (
                1,
                "",
                "failed to initialize postgamma_guc_restore.expected_buffers to 128",
                "parallel GUC restoration failed",
            ),
            (0, "128|command line\n128|command line", "", "did not launch a worker"),
            (
                0,
                "128|command line\nWorkers Launched: 1\n16384|default",
                "",
                "value or source was not inherited/preserved",
            ),
            (
                0,
                "128|override\nWorkers Launched: 1\n128|override",
                "",
                "value or source was not inherited/preserved",
            ),
        )
        for returncode, stdout, stderr, error in cases:
            with self.subTest(stdout=stdout, stderr=stderr):
                result = check_threaded_runtime.QueryResult(
                    returncode, 0.01, stdout, stderr
                )
                with patch.object(
                    check_threaded_runtime, "run_query", return_value=result
                ):
                    with self.assertRaisesRegex(
                        check_threaded_runtime.ThreadedRuntimeError, error
                    ):
                        check_threaded_runtime.validate_parallel_guc_restore(
                            ["psql"], {}, Path("/tmp/guc_restore_probe.so")
                        )

    def test_descriptor_growth_gate_rejects_workload_scaled_leaks(self) -> None:
        with self.assertRaisesRegex(
            check_thread_isolation.IsolationError,
            "main isolation workloads grew",
        ):
            check_thread_isolation.validate_fd_growth(
                33, 98, 64, "main isolation workloads", {"file": 70}
            )
        check_thread_isolation.validate_fd_growth(
            33, 44, 64, "main isolation workloads", {"file": 17}
        )

    def test_runtime_has_explicit_thread_exit_descriptor_cleanup(self) -> None:
        runtime = (PROJECT_ROOT / "runtime/src/postgres_backend_runtime.c").read_text(
            encoding="utf-8"
        )
        xlog_bridge = (
            PROJECT_ROOT
            / "runtime/include/postgamma/postgres_xlog_runtime_impl.h"
        ).read_text(encoding="utf-8")
        self.assertIn("FreeWaitEventSet(PG_STATE(FeBeWaitSet))", runtime)
        self.assertIn("postgamma_shutdown_xlog_file_access()", runtime)
        self.assertIn("POSTGAMMA_XLOG_STATE(openLogFile)", xlog_bridge)
        self.assertIn("ReleaseExternalFD()", xlog_bridge)

    def test_backend_registry_and_external_children_are_instance_owned(self) -> None:
        runtime = (PROJECT_ROOT / "runtime/src/postgres_backend_runtime.c").read_text(
            encoding="utf-8"
        )
        self.assertNotIn("PostgammaBackendRegistryInstance", runtime)
        self.assertNotIn("PostgammaExternalChildCount", runtime)
        self.assertNotIn("PostgammaExternalChildMutex", runtime)
        self.assertIn("postgamma_current_instance_runtime()", runtime)
        self.assertIn("state->registry", runtime)
        self.assertIn("postgamma_instance_runtime_notify_completion", runtime)

    def test_each_session_uses_private_names_and_state(self) -> None:
        first = check_thread_isolation.isolation_sql(3, 2048, 1000.0)
        second = check_thread_isolation.isolation_sql(4, 4096, 1000.0)
        for contract in (
            "SET LOCAL application_name='session-3'",
            "SET LOCAL ROLE postgamma_isolation_role_3",
            "CREATE TEMP TABLE session_marker",
            "PREPARE session_statement",
            "DECLARE session_cursor",
            "pg_advisory_xact_lock(3)",
            "1000.000000 - extract(epoch FROM clock_timestamp())",
            "isolation-ok-3",
        ):
            self.assertIn(contract, first)
        self.assertNotIn("session-3", second)

    def test_seeded_stress_statement_is_reproducible(self) -> None:
        import random

        left = check_thread_isolation.random_statement(random.Random(77), 9)
        right = check_thread_isolation.random_statement(random.Random(77), 9)
        self.assertEqual(left, right)
        self.assertIn("SET LOCAL application_name='stress-9'", left)

    def test_abrupt_disconnect_workload_forces_socket_output(self) -> None:
        source = Path(check_thread_isolation.__file__).read_text(encoding="utf-8")
        self.assertIn("COPY (SELECT repeat('x',4096)", source)
        self.assertIn("postgamma-abrupt-%", source)

    def test_error_and_mvcc_workloads_are_part_of_the_check_suite(self) -> None:
        source = Path(check_thread_isolation.__file__).read_text(encoding="utf-8")
        self.assertIn("validate_error_isolation(", source)
        self.assertIn("validate_mvcc_isolation(", source)
        self.assertIn("validate_auth_failure_isolation(", source)

    def test_threaded_runtime_exercises_external_child_ownership(self) -> None:
        source = Path(check_threaded_runtime.__file__).read_text(encoding="utf-8")
        self.assertIn("validate_external_command(", source)
        self.assertIn("TO PROGRAM 'cat > /dev/null'", source)

    def test_threaded_runtime_exercises_role_stack_limits(self) -> None:
        source = Path(check_threaded_runtime.__file__).read_text(encoding="utf-8")
        self.assertIn("validate_stack_depth(", source)
        self.assertIn("SET max_stack_depth='8MB'", source)
        self.assertIn("stack depth limit exceeded", source)


if __name__ == "__main__":
    unittest.main()
