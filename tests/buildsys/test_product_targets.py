"""Exercise public Make entry points with recording build/test backends."""

from __future__ import annotations

import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
BACKENDS = (
    "source-init", "doctor-ast", "doctor-full-test", "doctor-python",
    "doctor-native", "doctor-threaded", "doctor-sdk", "doctor-docs",
    "wasm-toolchain-setup", "wasm-toolchain-doctor", "wasm-browser-doctor",
    "doctor-wasm", "threaded-postgresql-build", "static-sdk-package",
    "python-wheel", "threaded-postgresql-check", "embedded-runtime-check",
    "python-package-check", "complete-source-check", "wasm-setup",
    "wasm-browser-setup", "wasm-generator-tools-check", "wasm-kernel-generate",
    "wasm-package", "wasm-check-full", "wasm-alpha-check",
)


class ProductTargetsTests(unittest.TestCase):
    def run_target(self, target, *assignments, fail="", environment=None):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            recorder = directory / "record.py"
            recorder.write_text(
                "import json, os, pathlib, sys\n"
                "path = pathlib.Path(os.environ['PRODUCT_TEST_LOG'])\n"
                "lock = path.with_suffix('.active')\n"
                "lock.mkdir()  # fail if two stages run concurrently\n"
                "try:\n"
                "    with path.open('a') as output:\n"
                "        output.write(json.dumps(dict(zip(\n"
                "            ('target', 'jobs', 'build', 'mode', 'sdk', 'extensions', 'keepGoing'),\n"
                "            sys.argv[1:]))) + '\\n')\n"
                "    if sys.argv[1] == os.environ.get('PRODUCT_TEST_FAIL'):\n"
                "        sys.exit(71)\n"
                "finally:\n"
                "    lock.rmdir()\n"
            )
            (directory / "Makefile").write_text(
                f"SHELL := /bin/bash\nROOT := {ROOT}\nPYTHON := {sys.executable}\n"
                "JOBS ?= 1\nBUILD_DIR ?= build\n"
                f"include {ROOT}/buildsys/wasm.mk\n"
                f"include {ROOT}/buildsys/make/products.mk\n"
                ".PHONY: " + " ".join(BACKENDS) + "\n"
                + " ".join(BACKENDS) + ":\n"
                f'\t@$(PYTHON) {recorder} "$@" "$(JOBS)" "$(BUILD_DIR)" '
                '"$(WASM_MODE)" "$(WASM_EMSDK_DIR)" "$(WASM_PACKAGE_EXTENSIONS)" '
                '"$(WASM_KEEP_GOING)"\n'
            )
            env = {name: value for name, value in os.environ.items()
                   if name not in ("MAKEFLAGS", "MFLAGS", "MAKELEVEL", "MAKEOVERRIDES")
                   and not name.startswith(("WASM_", "POSTGAMMA_WASM_"))}
            env.update(environment or {})
            log = directory / "calls.jsonl"
            env.update(PRODUCT_TEST_LOG=str(log), PRODUCT_TEST_FAIL=fail)
            result = subprocess.run(
                ["make", "--no-print-directory", "-j8", target, "JOBS=2",
                 "BUILD_DIR=custom-output", "WASM_EMSDK_DIR=/custom/sdk", *assignments],
                cwd=directory, env=env, capture_output=True, text=True, timeout=30,
            )
            rows = [json.loads(line) for line in log.read_text().splitlines()] if log.exists() else []
            return result, rows

    def test_native_builds_and_tests_prepare_sources_and_forward_options(self):
        for target, backend in (
            ("server-build", "threaded-postgresql-build"),
            ("c-build", "static-sdk-package"),
            ("python-build", "python-wheel"),
            ("server-test", "threaded-postgresql-check"),
            ("c-test", "embedded-runtime-check"),
            ("python-test", "python-package-check"),
        ):
            with self.subTest(target=target):
                result, rows = self.run_target(target)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual([row["target"] for row in rows], ["source-init", backend])
                self.assertTrue(all(row["jobs"] == "2" and row["build"] == "custom-output"
                                    for row in rows), rows)

    def test_wasm_build_prepares_generates_and_packages_in_release(self):
        result, rows = self.run_target("wasm-build")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual([row["target"] for row in rows], [
            "source-init", "wasm-toolchain-setup", "doctor-ast", "wasm-toolchain-doctor",
            "wasm-generator-tools-check", "wasm-kernel-generate", "wasm-package",
        ])
        self.assertEqual([row["mode"] for row in rows[-2:]], ["release", "release"])
        self.assertTrue(all(row["jobs"] == "2" and row["build"] == "custom-output"
                            and row["sdk"] == "/custom/sdk" for row in rows), rows)
        self.assertEqual(rows[-1]["extensions"], "standard")

    def test_wasm_build_honors_explicit_mode_and_extensions(self):
        for mode in ("debug", "release"):
            for source in ("command", "environment"):
                with self.subTest(mode=mode, source=source):
                    args = [f"WASM_MODE={mode}"] if source == "command" else []
                    env = {"WASM_MODE": mode} if source == "environment" else {}
                    result, rows = self.run_target(
                        "wasm-build", *args, "WASM_PACKAGE_EXTENSIONS=none", environment=env)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertEqual([row["mode"] for row in rows[-2:]], [mode, mode])
                    self.assertEqual(rows[-1]["extensions"], "none")

    def test_wasm_build_stops_after_any_failed_prerequisite(self):
        stages = ["source-init", "wasm-toolchain-setup", "doctor-ast", "wasm-toolchain-doctor",
                  "wasm-generator-tools-check", "wasm-kernel-generate", "wasm-package"]
        for index, stage in enumerate(stages):
            with self.subTest(stage=stage):
                result, rows = self.run_target("wasm-build", fail=stage)
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual([row["target"] for row in rows], stages[:index + 1])

    def test_full_test_preflights_then_runs_native_and_wasm_without_alpha_soak(self):
        result, rows = self.run_target("full-test", "WASM_MODE=debug")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual([row["target"] for row in rows], [
            "source-init", "wasm-setup", "wasm-browser-setup", "doctor-full-test", "doctor-python", "doctor-docs",
            "doctor-ast", "wasm-toolchain-doctor", "wasm-generator-tools-check", "wasm-browser-doctor",
            "complete-source-check", "source-init", "wasm-setup", "wasm-browser-setup",
            "wasm-check-full",
        ])
        self.assertEqual(rows[-1]["mode"], "release")
        self.assertEqual(rows[-1]["keepGoing"], "1")
        self.assertTrue(all(row["jobs"] == "2" and row["build"] == "custom-output"
                            for row in rows), rows)

    def test_full_test_prepare_does_not_build_kernel_or_run_acceptance(self):
        result, rows = self.run_target("full-test-prepare")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual([row["target"] for row in rows], [
            "source-init", "wasm-setup", "wasm-browser-setup", "doctor-full-test", "doctor-python", "doctor-docs",
            "doctor-ast", "wasm-toolchain-doctor", "wasm-generator-tools-check", "wasm-browser-doctor",
        ])

    def test_full_test_propagates_environment_native_and_browser_failures(self):
        for stage in ("doctor-full-test", "doctor-docs", "wasm-browser-setup", "wasm-toolchain-doctor",
                      "wasm-generator-tools-check", "complete-source-check", "wasm-check-full"):
            with self.subTest(stage=stage):
                result, rows = self.run_target("full-test", fail=stage)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(stage, [row["target"] for row in rows])
                if stage not in ("complete-source-check", "wasm-check-full"):
                    self.assertNotIn("complete-source-check", [row["target"] for row in rows])

    def test_product_doctors_only_check_their_installed_dependencies(self):
        for target, expected in (
            ("server-doctor", ["doctor-threaded"]),
            ("c-doctor", ["doctor-sdk"]),
            ("python-doctor", ["doctor-python"]),
            ("docs-doctor", ["doctor-docs"]),
            ("wasm-doctor", ["doctor-ast", "wasm-toolchain-doctor", "wasm-generator-tools-check"]),
        ):
            with self.subTest(target=target):
                result, rows = self.run_target(target)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual([row["target"] for row in rows], expected)

    def test_global_doctors_cover_wasm_without_installing_and_retain_failures(self):
        for target, first in (("doctor", ["doctor-native"]),
                              ("full-test-doctor", ["doctor-full-test", "doctor-python", "doctor-docs"])):
            expected = first + ["doctor-ast", "wasm-toolchain-doctor",
                                "wasm-generator-tools-check", "wasm-browser-doctor"]
            for failed in ("", first[0], "wasm-browser-doctor"):
                with self.subTest(target=target, failed=failed):
                    result, rows = self.run_target(target, fail=failed)
                    self.assertEqual(result.returncode == 0, not failed, result.stderr)
                    self.assertEqual([row["target"] for row in rows], expected)


if __name__ == "__main__":
    unittest.main()
