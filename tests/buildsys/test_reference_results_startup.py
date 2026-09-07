"""Exercise reference-server socket paths and persistent failure diagnostics."""

from __future__ import annotations

import re
import socket
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch


PROJECT_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(PROJECT_ROOT / "buildsys"))

import check_chunked_results  # noqa: E402
import check_copy_streaming  # noqa: E402
import check_ordered_results  # noqa: E402
from check_embedded_lifecycle import LifecycleCheckError  # noqa: E402


CHECKERS = (
    (check_ordered_results, check_ordered_results.OrderedResultsCheckError),
    (check_chunked_results, check_chunked_results.ChunkedResultsCheckError),
    (check_copy_streaming, check_copy_streaming.CopyCheckError),
)


@unittest.skipUnless(hasattr(socket, "AF_UNIX"), "Unix sockets are required")
class ReferenceResultsStartupTests(unittest.TestCase):
    def exercise_reference(self, checker, error_type, *, fail_start: bool) -> None:
        with (
            tempfile.TemporaryDirectory(prefix="pgm-ref-test-") as temp,
            socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as listener,
        ):
            root = Path(temp)
            build = root / "postgres-build"
            build.mkdir()
            driver = root / "reference-driver"
            driver.touch()
            reports = root / "reports"
            args = checker.parser().parse_args([
                "reference", "--postgres-build", str(build),
                "--driver", str(driver),
                "--work-root", str(root / ("nested-" * 20)),
                "--stdout", str(reports / "stdout"),
                "--stderr", str(reports / "stderr"),
                "--server-log", str(reports / "server.log"),
                "--output", str(reports / "evidence.json"),
            ])
            observed = {}

            def run_command(arguments, **kwargs):
                program = Path(arguments[0]).name
                if program == "make":
                    destination = next(
                        value.removeprefix("DESTDIR=")
                        for value in arguments if value.startswith("DESTDIR=")
                    )
                    postgres = Path(destination) / "usr/local/pgsql/bin/postgres"
                    postgres.parent.mkdir(parents=True)
                    postgres.touch()
                elif program == "initdb":
                    data = Path(arguments[arguments.index("--pgdata") + 1])
                    data.mkdir()
                    (data / "postgresql.conf").write_text("", encoding="utf-8")
                    observed["data"] = data
                elif program == "pg_ctl" and arguments[-1] == "start":
                    config = (observed["data"] / "postgresql.conf").read_text(
                        encoding="utf-8"
                    )
                    socket_match = re.search(
                        r"unix_socket_directories = '([^']+)'", config
                    )
                    port_match = re.search(r"port = (\d+)", config)
                    self.assertIsNotNone(socket_match)
                    self.assertIsNotNone(port_match)
                    observed["socket"] = Path(socket_match[1])
                    observed["address"] = str(
                        observed["socket"] / f".s.PGSQL.{port_match[1]}"
                    )
                    log = Path(arguments[arguments.index("-l") + 1])
                    observed["log"] = log
                    if fail_start:
                        log.write_text(
                            "FATAL: injected startup failure\n", encoding="utf-8"
                        )
                        raise LifecycleCheckError("reference startup failed")
                    listener.bind(observed["address"])
                    listener.listen(1)
                    (observed["data"] / "postmaster.pid").write_text(
                        "1234\n", encoding="utf-8"
                    )
                    log.write_text("LOG: server ready\n", encoding="utf-8")
                elif program == "pg_ctl" and arguments[-1] == "stop":
                    self.assertTrue(observed["socket"].is_dir())
                    listener.close()
                    observed["stopped"] = True
                    with observed["log"].open("a", encoding="utf-8") as log:
                        log.write("LOG: server stopped\n")
                elif arguments[0] == str(driver):
                    address = str(Path(arguments[1]) / f".s.PGSQL.{arguments[2]}")
                    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
                        client.connect(address)
                    observed["connected"] = True
                    return subprocess.CompletedProcess(
                        arguments, 1, "", "injected driver failure"
                    )
                else:
                    self.fail(f"unexpected reference command: {arguments}")
                return subprocess.CompletedProcess(arguments, 0, "", "")

            expected_error = LifecycleCheckError if fail_start else error_type
            expected_message = (
                "reference startup failed" if fail_start else "reference driver failed"
            )
            with (
                patch.object(checker, "run_checked", side_effect=run_command),
                self.assertRaisesRegex(expected_error, expected_message),
            ):
                checker.run_reference(args)

            self.assertFalse(observed["data"].parent.exists())
            self.assertFalse(observed["socket"].exists())
            self.assertFalse(args.output.exists())
            log_text = args.server_log.read_text(encoding="utf-8")
            if fail_start:
                self.assertIn("FATAL: injected startup failure", log_text)
                self.assertNotIn("connected", observed)
                self.assertNotIn("stopped", observed)
            else:
                self.assertTrue(observed["connected"])
                self.assertTrue(observed["stopped"])
                self.assertIn("LOG: server stopped", log_text)

    def test_socket_connects_from_deep_work_root(self) -> None:
        for checker, error_type in CHECKERS:
            with self.subTest(checker=checker.__name__):
                self.exercise_reference(checker, error_type, fail_start=False)

    def test_startup_failure_keeps_log_after_cluster_cleanup(self) -> None:
        for checker, error_type in CHECKERS:
            with self.subTest(checker=checker.__name__):
                self.exercise_reference(checker, error_type, fail_start=True)


if __name__ == "__main__":
    unittest.main()
