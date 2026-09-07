"""Tests for the stable embedded public C API evidence validator."""

from __future__ import annotations

import subprocess
import sys
import unittest
from pathlib import Path
from unittest import mock


PROJECT_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(PROJECT_ROOT / "buildsys"))

import check_embedded_public_api  # noqa: E402


def valid_marker() -> str:
    return (
        "POSTGAMMA_KERNEL_PUBLIC_API abi=65539 capabilities=112639 postgres=19 "
        "typed=true binary=true "
        "error=true settings=true notice=true notification=true cancel=true "
        "timeout=true timeout_cleanup_bounded=true "
        "caller_driven=true waitable=true request_threads=0 "
        "provider=pooled executor_workers=4 "
        "session_state=true guc_state=true temp_state=true prepared_state=true "
        "transaction_state=true portal_state=true "
        "holdable_cursor_migration=true advisory_pinning=true "
        "shell_process_rejected=true "
        "cancel_elapsed_ms=8 timeout_elapsed_ms=9 concurrent_connections=4 "
        "error_contract=true "
        "ownership=true creation_umask=true close_retry=true recovery=true "
        "phase=closed\n"
    )


def valid_runtime() -> str:
    return (
        "LOG: POSTGAMMA_RUNTIME backend_model=thread threads_started=true "
        "provider=pooled "
        "role_process_launches=0 forbidden_process_launch_attempts=0 "
        "unsupported_role_requests=0 client_threads_started=4 "
        "client_threads_peak=4 role_completions=11 role_threads_active=0 "
        "pooled_worker_threads=4 client_quantums=20 quantum_yields=16 "
        "carrier_migrations=12 runnable_sessions=0 running_quantums=0 "
        "pinned_sessions=0 blocked_sessions=0 execution_tokens_active=0 "
        "execution_tokens_peak=4 execution_token_budget=4 "
        "execution_token_rejections=0\n"
    )


class EmbeddedPublicApiEvidenceTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.expected_symbols = check_embedded_public_api.load_expected_public_symbols(
            PROJECT_ROOT / "manifests/api/c-public-api.json",
            PROJECT_ROOT / "manifests/api/c-core-abi-v1.json",
        )

    def test_accepts_complete_public_marker(self) -> None:
        values = check_embedded_public_api.parse_marker(valid_marker())
        self.assertEqual(values["cancel_elapsed_ms"], 8)
        self.assertEqual(values["timeout_elapsed_ms"], 9)
        self.assertEqual(values["concurrent_connections"], "4")

    def test_rejects_slow_cancellation(self) -> None:
        marker = valid_marker().replace(
            "cancel_elapsed_ms=8", "cancel_elapsed_ms=2001"
        )
        with self.assertRaisesRegex(
            check_embedded_public_api.PublicApiCheckError,
            "within 2 seconds",
        ):
            check_embedded_public_api.parse_marker(marker)

    def test_rejects_unbounded_timeout_cleanup(self) -> None:
        marker = valid_marker().replace(
            "timeout_elapsed_ms=9", "timeout_elapsed_ms=2001"
        )
        with self.assertRaisesRegex(
            check_embedded_public_api.PublicApiCheckError,
            "timeout cleanup did not complete within 2 seconds",
        ):
            check_embedded_public_api.parse_marker(marker)

    def test_accepts_clean_concurrent_runtime(self) -> None:
        report = check_embedded_public_api.audit_runtime(valid_runtime())
        self.assertEqual(report["client_threads_peak"], 4)
        self.assertEqual(report["role_threads_active_after_close"], 0)

    def test_rejects_fatal_backend_cleanup(self) -> None:
        with self.assertRaisesRegex(
            check_embedded_public_api.PublicApiCheckError,
            "fatal backend error",
        ):
            check_embedded_public_api.audit_runtime(
                valid_runtime() + "FATAL: leaked client role\n"
            )

    def test_accepts_thread_only_host_safe_trace(self) -> None:
        driver = Path("/tmp/public-api-driver")
        trace = (
            '100 execve("/tmp/public-api-driver", '
            '["/tmp/public-api-driver"], 0x0) = 0\n'
            "100 clone3({flags=CLONE_VM|CLONE_THREAD}, 88) = 101\n"
        )
        report = check_embedded_public_api.audit_trace(trace, driver)
        self.assertEqual(report["thread_clone_calls"], 1)
        self.assertEqual(report["host_signal_delivery_calls"], 0)

    def test_rejects_network_endpoint(self) -> None:
        driver = Path("/tmp/public-api-driver")
        trace = (
            '100 execve("/tmp/public-api-driver", '
            '["/tmp/public-api-driver"], 0x0) = 0\n'
            "100 clone3({flags=CLONE_VM|CLONE_THREAD}, 88) = 101\n"
            "101 socket(AF_UNIX, SOCK_STREAM, 0) = 7\n"
        )
        with self.assertRaisesRegex(
            check_embedded_public_api.PublicApiCheckError,
            "network endpoint",
        ):
            check_embedded_public_api.audit_trace(trace, driver)

    def test_rejects_process_timer(self) -> None:
        driver = Path("/tmp/public-api-driver")
        trace = (
            '100 execve("/tmp/public-api-driver", '
            '["/tmp/public-api-driver"], 0x0) = 0\n'
            "100 clone3({flags=CLONE_VM|CLONE_THREAD}, 88) = 101\n"
            "101 setitimer(ITIMER_REAL, {it_value={tv_sec=10}}, NULL) = 0\n"
        )
        with self.assertRaisesRegex(
            check_embedded_public_api.PublicApiCheckError,
            "process-global host state",
        ):
            check_embedded_public_api.audit_trace(trace, driver)

    def test_current_header_contains_only_public_types(self) -> None:
        headers = [
            PROJECT_ROOT / "embedded-c/include/postgamma/postgamma.h",
            PROJECT_ROOT / "embedded-c/include/postgamma/postgamma_arrow.h",
        ]
        report = check_embedded_public_api.audit_header(
            headers, self.expected_symbols
        )
        self.assertEqual(report["postgres_headers"], 0)
        self.assertEqual(
            report["implemented_declaration_count"],
            len(self.expected_symbols),
        )
        self.assertEqual(report["candidate_declaration_count"], 0)

    def test_accepts_caller_driven_frontend_source(self) -> None:
        source = PROJECT_ROOT / "embedded-c/src/postgamma.c"
        report = check_embedded_public_api.audit_frontend_source(source)
        self.assertTrue(report["caller_driven"])
        self.assertEqual(report["request_thread_creation_sites"], 0)

    def test_rejects_request_thread_creation(self) -> None:
        with mock.patch.object(
            Path, "read_text", return_value="pthread_create(&worker, 0, run, 0);"
        ):
            with self.assertRaisesRegex(
                check_embedded_public_api.PublicApiCheckError,
                "not caller-driven",
            ):
                check_embedded_public_api.audit_frontend_source(
                    Path("/tmp/postgamma.c")
                )

    def test_accepts_exact_dynamic_symbol_surface(self) -> None:
        lines = ["0000000000000000 A POSTGAMMA_1.0"]
        lines.extend(
            f"0000000000001000 T {name}@@POSTGAMMA_1.0"
            for name in sorted(self.expected_symbols)
        )
        completed = subprocess.CompletedProcess(
            args=[], returncode=0, stdout="\n".join(lines) + "\n", stderr=""
        )
        with mock.patch.object(
            check_embedded_public_api, "run_checked", return_value=completed
        ):
            report = check_embedded_public_api.audit_symbols(
                "nm", Path("/tmp/libpostgamma.so"), self.expected_symbols
            )
        self.assertEqual(report["postgres_symbols_exported"], 0)
        self.assertEqual(
            report["exported_symbol_count"],
            len(self.expected_symbols),
        )


if __name__ == "__main__":
    unittest.main()
