"""Tests for the runtime events diagnostics and routed-event evidence gate."""

from __future__ import annotations

import sys
import unittest
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(PROJECT_ROOT / "buildsys"))

import check_runtime_events  # noqa: E402


def valid_marker(*, connections: int = 1000, trace: bool = False) -> str:
    required = " ".join(
        f"{name}={value}"
        for name, value in check_runtime_events.REQUIRED_MARKER_VALUES.items()
    )
    return (
        f"{check_runtime_events.MARKER} {required} "
        f"connections={connections} fd_growth={connections * 5} "
        f"overflow_reported=9 trace={'true' if trace else 'false'}\n"
    )


def valid_runtime(*, connections: int = 1000) -> str:
    return (
        "LOG: POSTGAMMA_RUNTIME backend_model=thread host_pid=1234 "
        "provider=pooled threads_started=true role_process_launches=0 "
        "forbidden_process_launch_attempts=0 unsupported_role_requests=0 "
        "role_threads_active=0 client_threads_started=4 client_threads_peak=2 "
        f"role_completions={connections + 11} pooled_worker_threads=4 "
        f"client_quantums={connections * 2 + 20} "
        f"quantum_yields={connections + 16} carrier_migrations=12 "
        "runnable_sessions=0 running_quantums=0 running_quantums_peak=2 "
        "pinned_sessions=0 blocked_sessions=0 execution_tokens_active=0 "
        "execution_tokens_peak=2 execution_token_budget=4 "
        "execution_token_rejections=0\n"
        "FATAL: unrecognized configuration parameter "
        '"postgamma_missing_guc"\n'
        'FATAL: parameter "max_connections" cannot be changed without '
        "restarting the server\n"
    )


class EventsEvidenceTests(unittest.TestCase):
    def test_accepts_normal_scale_marker(self) -> None:
        marker = check_runtime_events.parse_marker(
            valid_marker(), expected_connections=1000, trace=False
        )
        self.assertEqual(marker["connections"], 1000)
        self.assertEqual(marker["fd_growth"], 5000)

    def test_accepts_trace_marker(self) -> None:
        marker = check_runtime_events.parse_marker(
            valid_marker(connections=32, trace=True),
            expected_connections=32,
            trace=True,
        )
        self.assertEqual(marker["trace"], "true")

    def test_rejects_missing_exact_once_proof(self) -> None:
        marker = valid_marker().replace("exact_once=true", "exact_once=false")
        with self.assertRaisesRegex(
            check_runtime_events.EventsCheckError, "exact_once"
        ):
            check_runtime_events.parse_marker(
                marker, expected_connections=1000, trace=False
            )

    def test_rejects_fd_growth_above_reviewed_baseline(self) -> None:
        marker = valid_marker().replace("fd_growth=5000", "fd_growth=5001")
        with self.assertRaisesRegex(
            check_runtime_events.EventsCheckError, "FD growth"
        ):
            check_runtime_events.parse_marker(
                marker, expected_connections=1000, trace=False
            )

    def test_accepts_quiescent_pooled_runtime(self) -> None:
        runtime = check_runtime_events.parse_runtime(
            valid_runtime(), expected_connections=1000
        )
        self.assertEqual(runtime["pooled_worker_threads"], 4)
        self.assertEqual(runtime["running_quantums"], "0")

    def test_rejects_retained_idle_quantum(self) -> None:
        runtime = valid_runtime().replace(
            "running_quantums=0", "running_quantums=1"
        )
        with self.assertRaisesRegex(
            check_runtime_events.EventsCheckError, "running_quantums"
        ):
            check_runtime_events.parse_runtime(runtime, expected_connections=1000)

    def test_rejects_unexpected_fatal(self) -> None:
        runtime = valid_runtime() + "FATAL: unexpected failure\n"
        with self.assertRaisesRegex(
            check_runtime_events.EventsCheckError, "unexpected fatal"
        ):
            check_runtime_events.parse_runtime(runtime, expected_connections=1000)

    def test_current_sources_have_o1_routed_waitable(self) -> None:
        audit = check_runtime_events.audit_event_routing(
            PROJECT_ROOT / "embedded-c/src/postgamma_events.c",
            PROJECT_ROOT / "embedded-c/src/postgamma.c",
            PROJECT_ROOT / "runtime/src/postgres_backend_runtime.c",
            PROJECT_ROOT / "manifests/postgresql/backend-quantum.json",
        )
        self.assertEqual(audit["instance_waitable_creation_sites"], 1)
        self.assertEqual(audit["per_connection_waitable_creation_sites"], 0)
        self.assertTrue(audit["notify_installed_after_registration"])
        self.assertTrue(audit["buffered_protocol_input_guard"])

    def test_rejects_notify_before_registration(self) -> None:
        source = """
        void
        pgm_connection_open(void)
        {
            postgamma_private_libpq_set_notify();
            postgamma_public_event_register_connection();
        }
        """
        with self.assertRaisesRegex(
            check_runtime_events.EventsCheckError, "before event registration"
        ):
            check_runtime_events.audit_connection_open_order(source)


if __name__ == "__main__":
    unittest.main()
