"""Tests for deterministic source-build environment diagnostics."""

from __future__ import annotations

import subprocess
import sys
import tempfile
import unittest
from contextlib import redirect_stdout
from io import StringIO
from pathlib import Path
from unittest import mock


PROJECT_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(PROJECT_ROOT / "buildsys"))

import doctor  # noqa: E402


class DoctorTests(unittest.TestCase):
    def test_timeout_reason_survives_long_command_diagnostics(self) -> None:
        command = [sys.executable, "-c", "import time; time.sleep(30)", "x" * 300]
        result = doctor.run(command, timeout=0.1)
        self.assertEqual(result.returncode, 124)
        self.assertEqual(result.args, command)
        self.assertIn(
            "timed out after 0.1 seconds", doctor.concise_failure(result.stdout)
        )

    def test_ast_compile_budget_accepts_slow_tools_and_rejects_hangs(self) -> None:
        version = str(doctor.LLVM_MAJORS[0]) + ".0.0"
        for compile_seconds, expected_status in ((30.0, "ok"), (150.0, "error")):
            with self.subTest(compile_seconds=compile_seconds):
                executed = []

                def run_command(command, timeout=20.0):
                    if "--version" in command:
                        return subprocess.CompletedProcess(command, 0, version)
                    if any(str(part).endswith("probe.cpp") for part in command):
                        if compile_seconds > timeout:
                            return subprocess.CompletedProcess(
                                command, 124, "command timed out"
                            )
                    elif len(command) == 1 and Path(command[0]).name == "probe":
                        executed.append(command)
                    else:
                        self.fail(f"unexpected probe command: {command}")
                    return subprocess.CompletedProcess(command, 0, "")

                with tempfile.TemporaryDirectory() as temporary:
                    directory = Path(temporary)
                    reporter = doctor.Reporter()
                    with mock.patch.multiple(
                        doctor,
                        find_llvm_config=mock.Mock(return_value="llvm-config"),
                        find_clangxx=mock.Mock(return_value="clang++"),
                        find_clang_resource_dir=mock.Mock(return_value=temporary),
                        find_clang_cpp_library=mock.Mock(return_value="-lclang-cpp"),
                        llvm_arguments=mock.Mock(return_value=([], None)),
                        run=mock.Mock(side_effect=run_command),
                    ):
                        doctor.inspect_ast({"ast"}, directory, directory, reporter)
                probe = next(
                    finding for finding in reporter.findings
                    if finding.key == "clang-probe"
                )
                self.assertEqual(probe.status, expected_status)
                self.assertEqual(bool(executed), expected_status == "ok")

    def test_command_override_is_parsed_without_a_shell(self) -> None:
        self.assertEqual(
            doctor.command_tokens("ccache 'clang-19' --target=x86_64-linux-gnu"),
            ("ccache", "clang-19", "--target=x86_64-linux-gnu"),
        )
        self.assertEqual(doctor.command_tokens("clang '"), ())

    def test_default_postgresql_dependencies_are_enabled(self) -> None:
        features, errors = doctor.required_configure_features({})
        self.assertEqual(errors, [])
        self.assertTrue(features["icu"])
        self.assertTrue(features["readline"])
        self.assertTrue(features["zlib"])
        self.assertFalse(features["ssl"])
        self.assertFalse(features["lz4"])

    def test_configure_switches_control_native_dependency_checks(self) -> None:
        features, errors = doctor.required_configure_features(
            {
                "PG_CONFIGURE_ARGS": (
                    "--without-icu --without-readline --without-zlib "
                    "--with-ssl=openssl --with-lz4"
                )
            }
        )
        self.assertEqual(errors, [])
        self.assertFalse(features["icu"])
        self.assertFalse(features["readline"])
        self.assertFalse(features["zlib"])
        self.assertTrue(features["ssl"])
        self.assertTrue(features["lz4"])

    def test_tree_specific_configure_switch_can_restore_a_feature(self) -> None:
        features, errors = doctor.required_configure_features(
            {
                "PG_CONFIGURE_ARGS": "--without-icu",
                "PG_EMBEDDED_CONFIGURE_ARGS": "--with-icu",
            }
        )
        self.assertEqual(errors, [])
        self.assertTrue(features["icu"])

    def test_malformed_configure_arguments_are_reported(self) -> None:
        _features, errors = doctor.required_configure_features(
            {"PG_CONFIGURE_ARGS": "--with-includes='unterminated"}
        )
        self.assertEqual(len(errors), 1)
        self.assertIn("PG_CONFIGURE_ARGS", errors[0])

    def test_missing_submodules_have_one_actionable_finding(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            reporter = doctor.Reporter()
            doctor.inspect_checkout(Path(temporary), None, reporter)
        failures = reporter.failures()
        self.assertEqual([finding.key for finding in failures], ["submodules"])
        self.assertIn("postgres", failures[0].detail)
        self.assertIn("third_party/pgvector", failures[0].detail)

    def test_environment_warns_about_nonportable_flags(self) -> None:
        reporter = doctor.Reporter()
        doctor.inspect_environment(
            {"environment"},
            PROJECT_ROOT,
            {"CFLAGS": "-O2 -Werror -march=native"},
            reporter,
        )
        warning = next(
            finding
            for finding in reporter.findings
            if finding.key == "risky-overrides"
        )
        self.assertEqual(warning.status, "warning")
        self.assertIn("host-CPU-specific", warning.detail)
        self.assertIn("upstream warnings", warning.detail)

    def test_package_suggestions_are_deduplicated(self) -> None:
        failures = [
            doctor.Finding("base", "cc", "missing", "C compiler", "missing"),
            doctor.Finding("base", "cxx", "missing", "C++ compiler", "missing"),
        ]
        packages: set[str] = set()
        for finding in failures:
            packages.update(doctor.APT_PACKAGES[finding.key].split())
        self.assertEqual(packages, {"build-essential"})

    def test_public_profiles_have_explicit_artifact_names(self) -> None:
        self.assertEqual(
            doctor.PROFILE_GROUPS["threaded"],
            {
                "host",
                "checkout",
                "base",
                "ast",
                "native",
                "resources",
                "environment",
            },
        )
        self.assertEqual(doctor.PROFILE_GROUPS["sdk"], doctor.PROFILE_GROUPS["build"])
        self.assertEqual(
            doctor.PROFILE_GROUPS["all"],
            doctor.PROFILE_GROUPS["release"],
        )

    def test_unsupported_tool_version_gets_versioned_apt_guidance(self) -> None:
        failures = [
            doctor.Finding(
                "ast", "llvm-config", "error", "llvm-config", "unsupported version"
            ),
            doctor.Finding(
                "ast", "llvm-clang-pair", "error", "pairing", "unsupported version"
            ),
        ]
        output = StringIO()
        release = {
            "ID": "ubuntu",
            "NAME": "Ubuntu",
            "VERSION_ID": "22.04",
            "VERSION_CODENAME": "jammy",
        }
        with mock.patch.object(doctor, "read_os_release", return_value=release):
            with mock.patch.object(doctor.platform, "machine", return_value="x86_64"):
                with redirect_stdout(output):
                    doctor.print_hints(failures)
        message = output.getvalue()
        self.assertNotIn("Suggested packages:", message)
        self.assertIn("Detected platform: Ubuntu 22.04 (jammy), x86_64", message)
        self.assertIn("curl -fsSLO https://apt.llvm.org/llvm.sh", message)
        self.assertIn("sudo ./llvm.sh 22", message)
        self.assertIn(
            "sudo apt-get install -y libclang-22-dev llvm-22-dev",
            message,
        )

    def test_missing_llvm_uses_versioned_apt_guidance(self) -> None:
        failures = [
            doctor.Finding("ast", "llvm-config", "missing", "llvm-config", "missing"),
            doctor.Finding("ast", "clang++", "missing", "clang++", "missing"),
        ]
        output = StringIO()
        release = {
            "ID": "debian",
            "NAME": "Debian GNU/Linux",
            "VERSION_ID": "12",
            "VERSION_CODENAME": "bookworm",
        }
        with mock.patch.object(doctor, "read_os_release", return_value=release):
            with redirect_stdout(output):
                doctor.print_hints(failures)
        message = output.getvalue()
        self.assertNotIn("Suggested packages:", message)
        self.assertIn("curl -fsSLO https://apt.llvm.org/llvm.sh", message)
        self.assertIn("sudo ./llvm.sh 22", message)
        self.assertIn(
            "sudo apt-get install -y libclang-22-dev llvm-22-dev",
            message,
        )

    def test_amazon_linux_is_detected_before_fedora_id_like(self) -> None:
        with mock.patch.object(
            doctor,
            "read_os_release",
            return_value={"ID": "amzn", "ID_LIKE": "fedora"},
        ):
            self.assertEqual(doctor.package_family(), "dnf-amazon")

    def test_amazon_linux_uses_versioned_llvm_and_python_guidance(self) -> None:
        failures = [
            doctor.Finding(
                "ast", "llvm-config", "error", "llvm-config", "unsupported version"
            ),
            doctor.Finding(
                "base", "python", "error", "Python", "unsupported version"
            ),
        ]
        output = StringIO()
        release = {"ID": "amzn", "NAME": "Amazon Linux", "VERSION_ID": "2023"}
        with mock.patch.object(doctor, "read_os_release", return_value=release):
            with redirect_stdout(output):
                doctor.print_hints(failures)
        message = output.getvalue()
        self.assertIn(
            "sudo dnf install -y clang18 clang18-devel llvm18-devel",
            message,
        )
        self.assertIn(
            "sudo dnf install python3.11 python3.11-devel",
            message,
        )
        self.assertIn("PYTHON=python3.11", message)
        self.assertNotIn("clang clang-devel llvm-devel", message)
        self.assertNotIn("python3-devel", message)

    def test_opensuse_uses_zypper_package_guidance(self) -> None:
        failures = [
            doctor.Finding("base", "find", "missing", "find", "missing"),
            doctor.Finding("ast", "llvm-config", "missing", "llvm-config", "missing"),
            doctor.Finding("ast", "clang++", "missing", "clang++", "missing"),
        ]
        output = StringIO()
        with mock.patch.object(
            doctor,
            "read_os_release",
            return_value={"ID": "opensuse-leap", "ID_LIKE": "suse opensuse"},
        ):
            self.assertEqual(doctor.package_family(), "zypper")
            with redirect_stdout(output):
                doctor.print_hints(failures)
        self.assertIn(
            "Suggested packages: sudo zypper install -y findutils",
            output.getvalue(),
        )
        self.assertIn(
            "sudo zypper install -y clang clang-devel llvm-devel",
            output.getvalue(),
        )

    def test_fedora_uses_dnf_llvm_guidance_for_a_version_error(self) -> None:
        failures = [
            doctor.Finding(
                "ast", "llvm-clang-pair", "error", "pairing", "version mismatch"
            )
        ]
        release = {
            "ID": "fedora",
            "NAME": "Fedora Linux",
            "VERSION_ID": "42",
            "VERSION_CODENAME": "Adams",
        }
        output = StringIO()
        with mock.patch.object(doctor, "read_os_release", return_value=release):
            with redirect_stdout(output):
                doctor.print_hints(failures)
        self.assertIn(
            "sudo dnf install -y clang clang-devel llvm-devel",
            output.getvalue(),
        )

    def test_unknown_apt_derivative_does_not_get_unverified_llvm_commands(
        self,
    ) -> None:
        failures = [
            doctor.Finding("ast", "llvm-config", "missing", "llvm-config", "missing")
        ]
        release = {
            "ID": "custom-linux",
            "ID_LIKE": "debian",
            "NAME": "Custom Linux",
            "VERSION_ID": "1",
        }
        output = StringIO()
        with mock.patch.object(doctor, "read_os_release", return_value=release):
            with redirect_stdout(output):
                doctor.print_hints(failures)
        message = output.getvalue()
        self.assertNotIn("sudo apt-get install", message)
        self.assertNotIn("llvm.sh", message)
        self.assertIn("No verified LLVM/Clang install command", message)

    def test_make_floor_matches_grouped_target_syntax(self) -> None:
        makefile = (PROJECT_ROOT / "Makefile").read_text(encoding="utf-8")
        self.assertIn("&:", makefile)
        self.assertGreaterEqual(doctor.MINIMUM_MAKE, (4, 3))

    def test_readline_probe_declares_file_before_legacy_headers(self) -> None:
        source = doctor.READLINE_PROBE_SOURCE
        self.assertLess(source.index("#include <stdio.h>"), source.index("readline.h"))

    def test_public_build_preflights_are_serialized(self) -> None:
        makefile = (PROJECT_ROOT / "Makefile").read_text(encoding="utf-8")
        self.assertIn(
            ".NOTPARALLEL: $(POSTGAMMA_PUBLIC_BUILD_GOALS) threaded-runtime-full-check",
            makefile,
        )

    def test_public_doctor_targets_name_their_scope(self) -> None:
        makefile = (PROJECT_ROOT / "Makefile").read_text(encoding="utf-8")
        self.assertIn("--require-profile all", makefile)
        self.assertIn("--require-profile threaded", makefile)
        self.assertIn("--require-profile sdk", makefile)
        self.assertIn(
            "threaded-postgresql-build: doctor-threaded generated-build",
            makefile,
        )
        self.assertIn("static-sdk-package: doctor-sdk", makefile)
        self.assertIn(
            "release-candidate: doctor $(RELEASE_CANDIDATE_RECEIPT)",
            makefile,
        )


if __name__ == "__main__":
    unittest.main()
