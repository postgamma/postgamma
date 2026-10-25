"""Tests for the runtime events diagnostics and routed-event evidence gate."""

from __future__ import annotations

import contextlib
import io
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock


PROJECT_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(PROJECT_ROOT / "buildsys"))

import check_runtime_events  # noqa: E402


def valid_marker(
    *, connections: int = 1000, trace: bool = False,
    request_timeout_seconds: int = 30,
) -> str:
    required = " ".join(
        f"{name}={value}"
        for name, value in check_runtime_events.REQUIRED_MARKER_VALUES.items()
    )
    return (
        f"{check_runtime_events.MARKER} {required} "
        f"connections={connections} fd_growth={connections * 5} "
        f"request_timeout_ms={request_timeout_seconds * 1000} "
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
    def test_invalid_budgets_preserve_existing_evidence(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            report = root / "report.json"
            report.write_text("existing evidence")
            arguments = ["check_runtime_events.py", "--output", str(report)]
            for name in (
                "postgres-build", "library", "driver", "event-source",
                "connection-source", "runtime-source", "quantum-manifest",
                "work-root", "stdout", "stderr", "trace",
            ):
                arguments.extend([f"--{name}", str(root / "unused")])
            for name, value in (
                ("timeout-seconds", "0"), ("timeout-seconds", "-1"),
                ("request-timeout-seconds", "0"),
                ("request-timeout-seconds", "-1"),
                ("request-timeout-seconds", "1.5"),
                ("request-timeout-seconds", "9223372036854776"),
            ):
                with (
                    self.subTest(name=name, value=value),
                    mock.patch.object(sys, "argv", arguments + [f"--{name}", value]),
                    contextlib.redirect_stderr(io.StringIO()) as stderr,
                    self.assertRaises(SystemExit) as failure,
                ):
                    check_runtime_events.main()
                self.assertEqual(failure.exception.code, 2)
                self.assertIn(name, stderr.getvalue())
                self.assertEqual(report.read_text(), "existing evidence")

    def test_rejects_driver_using_a_different_request_budget(self) -> None:
        with self.assertRaisesRegex(
            check_runtime_events.EventsCheckError, "request timeout"
        ):
            check_runtime_events.parse_marker(
                valid_marker(), expected_connections=1000, trace=False,
                request_timeout_seconds=300,
            )

    def test_driver_budgets_are_forwarded_and_recorded_without_changing_scale(self) -> None:
        for outer, request, overrides in (
            (300, 30, []),
            (1200, 300, ["--timeout-seconds", "1200",
                         "--request-timeout-seconds", "300"]),
        ):
            with self.subTest(outer=outer, request=request), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                library = root / "libpostgamma.so"
                driver = root / "events-driver"
                library.touch()
                driver.touch()
                output = root / "report.json"
                arguments = ["check_runtime_events.py"]
                paths = {
                    "postgres-build": root,
                    "library": library,
                    "driver": driver,
                    "event-source": PROJECT_ROOT / "embedded-c/src/postgamma_events.c",
                    "connection-source": PROJECT_ROOT / "embedded-c/src/postgamma.c",
                    "runtime-source": PROJECT_ROOT / "runtime/src/postgres_backend_runtime.c",
                    "quantum-manifest": PROJECT_ROOT / "manifests/postgresql/backend-quantum.json",
                    "work-root": root / "run",
                    "stdout": root / "events.stdout",
                    "stderr": root / "events.stderr",
                    "trace": root / "events.strace",
                    "output": output,
                }
                for name, path in paths.items():
                    arguments.extend([f"--{name}", str(path)])
                arguments.extend(overrides)

                def run_command(command: list[str], **_kwargs: object) -> subprocess.CompletedProcess[str]:
                    if command[0] == "make":
                        install = Path(command[-1].split("=", 1)[1])
                        postgres = install / "usr/local/pgsql/bin/postgres"
                        postgres.parent.mkdir(parents=True)
                        postgres.touch()
                        return subprocess.CompletedProcess(command, 0, "", "")
                    traced = command[0] == "strace"
                    connections = 32 if traced else 1000
                    if traced:
                        paths["trace"].write_text("")
                    return subprocess.CompletedProcess(
                        command, 0,
                        valid_marker(connections=connections, trace=traced,
                                     request_timeout_seconds=request),
                        valid_runtime(connections=connections),
                    )

                with (
                    mock.patch.object(sys, "argv", arguments),
                    mock.patch.object(check_runtime_events, "run_checked", side_effect=run_command) as run,
                    mock.patch.object(check_runtime_events, "audit_event_routing", return_value={}),
                    mock.patch.object(check_runtime_events, "audit_trace", return_value={}),
                    mock.patch.object(check_runtime_events, "raised_nofile_limit",
                                      return_value=contextlib.nullcontext({})),
                    contextlib.redirect_stdout(io.StringIO()),
                ):
                    self.assertEqual(check_runtime_events.main(), 0)
                self.assertEqual(len(run.call_args_list), 3)
                normal, traced = run.call_args_list[1:]
                self.assertEqual(normal.args[0][-2:], ["create", str(request)])
                self.assertEqual(traced.args[0][-3:], ["create", str(request), "trace"])
                self.assertEqual(normal.kwargs["timeout"], outer)
                self.assertEqual(traced.kwargs["timeout"], 180.0)
                report = json.loads(output.read_text())
                self.assertEqual(report["timeouts"], {
                    "normal_seconds": outer, "trace_seconds": 180,
                    "request_seconds": request,
                })
                self.assertEqual(report["scale"]["logical_connections"], 1000)
                self.assertEqual(report["trace"]["marker"]["connections"], 32)

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
