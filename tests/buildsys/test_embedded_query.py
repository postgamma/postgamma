"""Tests for the real embedded query evidence validator."""

from __future__ import annotations

import sys
import unittest
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(PROJECT_ROOT / "buildsys"))

import check_embedded_query  # noqa: E402


def valid_marker() -> str:
    return (
        "POSTGAMMA_KERNEL_QUERY generation=620001 backend_pid=1000000007 "
        "executor=pooled "
        "connected=true select_one=true error_sqlstate=22012 "
        "error_recovery=true extended_params=true dynamic_result=true "
        "binary_result=true cancellation=true cancel_dispatches=1 "
        "settings_precedence=true safety_policy=true "
        "transport=memory network_calls=0 "
        "secure_reads=11 secure_writes=4 socket_waits=5 "
        "supervisor_configured_stack_bytes=4194304 "
        "supervisor_configured_guard_bytes=65536 "
        "supervisor_native_stack_bytes=4194304 "
        "supervisor_native_guard_bytes=65536 "
        "supervisor_usable_stack_bytes=4128768 "
        "supervisor_actual_bounds=true "
        "active_transports_after_close=0 "
        "endpoint_references_after_close=0 phase=closed state=closed\n"
    )


class EmbeddedQueryEvidenceTests(unittest.TestCase):
    def test_accepts_unsafe_configuration_rejection(self) -> None:
        report = check_embedded_query.validate_policy_rejection(
            1,
            "",
            'FATAL: unsafe setting "restart_after_crash" is not allowed '
            "in embedded mode\nDETAIL: Value came from configuration file.\n",
        )
        self.assertEqual(report["origin"], "configuration file")

    def test_rejects_unsafe_configuration_acceptance(self) -> None:
        with self.assertRaisesRegex(
            check_embedded_query.QueryCheckError, "was accepted"
        ):
            check_embedded_query.validate_policy_rejection(0, "", "")

    def test_rejects_policy_error_without_origin(self) -> None:
        with self.assertRaisesRegex(
            check_embedded_query.QueryCheckError, "setting origin"
        ):
            check_embedded_query.validate_policy_rejection(
                1,
                "",
                'unsafe setting "restart_after_crash" is not allowed '
                "in embedded mode\n",
            )

    def test_accepts_complete_query_marker(self) -> None:
        values = check_embedded_query.parse_marker(valid_marker())
        self.assertEqual(values["error_sqlstate"], "22012")
        self.assertEqual(values["transport"], "memory")

    def test_uses_quantums_as_pooled_stack_observation_count(self) -> None:
        count = check_embedded_query.parse_client_quantum_count(
            "LOG: POSTGAMMA_RUNTIME provider=pooled client_quantums=17\n"
        )
        self.assertEqual(count, 17)

    def test_accepts_repeated_generation_scoped_cancel_dispatches(self) -> None:
        marker = valid_marker().replace(
            "cancel_dispatches=1", "cancel_dispatches=3"
        )
        values = check_embedded_query.parse_marker(marker)
        self.assertEqual(values["cancel_dispatches"], "3")

    def test_rejects_zero_protocol_activity(self) -> None:
        marker = valid_marker().replace("secure_reads=11", "secure_reads=0")
        with self.assertRaisesRegex(
            check_embedded_query.QueryCheckError, "secure_reads"
        ):
            check_embedded_query.parse_marker(marker)

    def test_rejects_inconsistent_supervisor_stack(self) -> None:
        marker = valid_marker().replace(
            "supervisor_usable_stack_bytes=4128768",
            "supervisor_usable_stack_bytes=4128767",
        )
        with self.assertRaisesRegex(
            check_embedded_query.QueryCheckError,
            "usable stack is inconsistent",
        ):
            check_embedded_query.parse_marker(marker)

    def test_accepts_thread_only_host_safe_trace(self) -> None:
        driver = Path("/tmp/kernel-query-driver")
        trace = (
            '100 execve("/tmp/kernel-query-driver", '
            '["/tmp/kernel-query-driver"], 0x0) = 0\n'
            "100 clone3({flags=CLONE_VM|CLONE_THREAD}, 88) = 101\n"
        )
        report = check_embedded_query.audit_trace(trace, driver)
        self.assertEqual(report["thread_clone_calls"], 1)
        self.assertEqual(report["network_endpoint_calls"], 0)

    def test_rejects_any_operating_system_socket(self) -> None:
        driver = Path("/tmp/kernel-query-driver")
        trace = (
            '100 execve("/tmp/kernel-query-driver", '
            '["/tmp/kernel-query-driver"], 0x0) = 0\n'
            "100 clone3({flags=CLONE_VM|CLONE_THREAD}, 88) = 101\n"
            "101 socket(AF_UNIX, SOCK_STREAM, 0) = 7\n"
        )
        with self.assertRaisesRegex(
            check_embedded_query.QueryCheckError, "network endpoint"
        ):
            check_embedded_query.audit_trace(trace, driver)

    def test_rejects_a_process_clone(self) -> None:
        driver = Path("/tmp/kernel-query-driver")
        trace = (
            '100 execve("/tmp/kernel-query-driver", '
            '["/tmp/kernel-query-driver"], 0x0) = 0\n'
            "100 clone(child_stack=NULL, flags=SIGCHLD) = 101\n"
        )
        with self.assertRaisesRegex(
            check_embedded_query.QueryCheckError, "without CLONE_THREAD"
        ):
            check_embedded_query.audit_trace(trace, driver)


if __name__ == "__main__":
    unittest.main()
