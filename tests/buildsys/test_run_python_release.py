"""Release orchestration and real small-wheel tool handoff tests.

The native fixture below has no database kernel and must fail product acceptance.
"""

from __future__ import annotations

import argparse
import hashlib
import importlib.util
import io
import json
import os
import platform
import shlex
import shutil
import subprocess
import sys
import tempfile
import textwrap
import unittest
import zipfile
from pathlib import Path
from unittest import mock


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "buildsys"))

import check_python_wheel  # noqa: E402
import run_python_release  # noqa: E402


def option(command: list[str], name: str) -> str:
    return command[command.index(name) + 1]


class ReleaseRunnerTests(unittest.TestCase):
    def setUp(self) -> None:
        temporary = tempfile.TemporaryDirectory(prefix="release runner ")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.kernel = self.root / "prebuilt kernel"
        self.kernel.mkdir()
        self.args = argparse.Namespace(
            static_library=self.kernel / "libpostgamma_python_abi1.a",
            link_options=self.kernel / "postgamma-static-libs.txt",
            static_receipt=self.kernel / "static-library-link.json",
            resource_root=self.kernel / "resource-pack",
            output_dir=self.root / "release output",
            platform=None, auditwheel="auditwheel", readelf="readelf",
        )
        for path in (self.args.static_library, self.args.link_options,
                     self.args.static_receipt):
            path.write_bytes(b"fixture input; not a product kernel\n")
        self.args.resource_root.mkdir()
        stdout = mock.patch("sys.stdout", new_callable=io.StringIO)
        self.stdout = stdout.start()
        self.addCleanup(stdout.stop)

    def test_pipeline_hands_repaired_bytes_to_pip_and_product_checks(self) -> None:
        with mock.patch.object(run_python_release, "run_stage") as stage:
            evidence = run_python_release.run_release(self.args)
        self.assertEqual([call.args[0] for call in stage.call_args_list],
                         ["build", "repair", "check"])
        build, repair, check = [list(call.args[1]) for call in stage.call_args_list]
        for command, script in ((build, "python/build_backend.py"),
                                (repair, "buildsys/repair_python_wheel.py"),
                                (check, "buildsys/check_python_wheel.py")):
            self.assertEqual(command[:2], [sys.executable, str(ROOT / script)])
        self.assertEqual(option(build, "--wheel-dir"), option(repair, "--wheel-dir"))
        self.assertEqual(option(build, "--receipt"), option(repair, "--receipt"))
        self.assertEqual(option(repair, "--output-dir"), option(check, "--wheel-dir"))
        self.assertEqual(option(repair, "--output-receipt"), option(check, "--receipt"))
        self.assertNotEqual(option(build, "--wheel-dir"), option(check, "--wheel-dir"))
        self.assertEqual(option(check, "--installer"), "pip")
        self.assertEqual(option(check, "--python"), sys.executable)
        self.assertEqual(option(check, "--require-platform"), "manylinux_2_17_x86_64")
        self.assertEqual(option(repair, "--platform"), option(check, "--require-platform"))
        self.assertEqual(option(check, "--baseline"), str(ROOT / "manifests/api/python-api-v1.json"))
        self.assertEqual(option(check, "--tests"), str(ROOT / "tests/python"))
        self.assertEqual(option(check, "--integration"), str(ROOT / "tests/python/integration_driver.py"))
        self.assertEqual(option(check, "--project-root"), str(ROOT))
        self.assertEqual(option(check, "--forbidden-prefix"), str(ROOT))
        self.assertEqual(option(check, "--output"), str(evidence))
        environment = stage.call_args_list[0].args[2]
        self.assertEqual(environment["POSTGAMMA_STATIC_LIBRARY"], str(self.args.static_library))
        self.assertEqual(environment["POSTGAMMA_STATIC_LINK_OPTIONS"], str(self.args.link_options))
        self.assertEqual(environment["POSTGAMMA_STATIC_RECEIPT"], str(self.args.static_receipt))
        self.assertEqual(environment["POSTGAMMA_RESOURCE_ROOT"], str(self.args.resource_root))

    def test_all_stages_use_the_baseline_epoch_without_mutating_the_caller(self) -> None:
        baseline = run_python_release.load_release_baseline(ROOT / "manifests/api/python-api-v1.json")
        expected = str(baseline["wheel"]["source_date_epoch"])
        for supplied in (None, "0", "946684800", "invalid"):
            with self.subTest(supplied=supplied):
                environment = dict(os.environ)
                environment.pop("SOURCE_DATE_EPOCH", None)
                if supplied is not None:
                    environment["SOURCE_DATE_EPOCH"] = supplied
                with mock.patch.dict(os.environ, environment, clear=True), \
                     mock.patch.object(run_python_release, "run_stage") as stage:
                    run_python_release.run_release(self.args)
                    self.assertEqual(os.environ.get("SOURCE_DATE_EPOCH"), supplied)
                self.assertEqual(len(stage.call_args_list), 3)
                for call in stage.call_args_list:
                    self.assertEqual(call.args[2]["SOURCE_DATE_EPOCH"], expected)

    def test_baseline_rejects_missing_or_unrepresentable_zip_epoch(self) -> None:
        baseline = json.loads((ROOT / "manifests/api/python-api-v1.json").read_text())
        path = self.root / "baseline.json"
        for epoch in (None, True, "315532800", 315532800.0, 0, 315532801, 4354819200):
            with self.subTest(epoch=epoch):
                baseline["wheel"]["source_date_epoch"] = epoch
                path.write_text(json.dumps(baseline))
                with self.assertRaisesRegex(check_python_wheel.WheelCheckError, "source_date_epoch"):
                    check_python_wheel.load_release_baseline(path)

    def test_stage_failures_stop_and_remove_old_pass_evidence(self) -> None:
        real_stage = run_python_release.run_stage
        for failed in ("build", "repair", "check"):
            with self.subTest(failed=failed):
                reports = self.args.output_dir / "reports"
                reports.mkdir(parents=True, exist_ok=True)
                (reports / "python-wheel.json").write_text('{"status":"pass"}')
                (self.args.output_dir / "keep.txt").write_text("unrelated output")

                def child(name, command, environment, directory):
                    code = ("import sys; print('stage stdout'); "
                            "print('stage stderr', file=sys.stderr); "
                            f"sys.exit({7 if name == failed else 0})")
                    real_stage(name, [sys.executable, "-c", code], environment, directory)

                with mock.patch.object(run_python_release, "run_stage", side_effect=child):
                    with self.assertRaisesRegex(run_python_release.ReleaseRunError,
                                                f"{failed} failed with exit status 7"):
                        run_python_release.run_release(self.args)
                logs = {path.name for path in reports.glob("*.log")}
                expected = {"build.log"}
                if failed != "build":
                    expected.add("repair.log")
                if failed == "check":
                    expected.add("check.log")
                self.assertEqual(logs, expected)
                self.assertFalse((reports / "python-wheel.json").exists())
                self.assertIn("stage stdout", (reports / f"{failed}.log").read_text())
                self.assertIn("stage stderr", (reports / f"{failed}.log").read_text())
                self.assertEqual((self.args.output_dir / "keep.txt").read_text(), "unrelated output")
                self.assertTrue(self.args.static_library.is_file())

    def test_missing_prebuilt_input_stops_without_starting_a_build(self) -> None:
        self.args.static_library.unlink()
        with mock.patch.object(run_python_release, "run_stage") as stage:
            with self.assertRaisesRegex(run_python_release.ReleaseRunError,
                                        "prebuilt kernel input is missing.*STATIC_LIBRARY"):
                run_python_release.run_release(self.args)
        stage.assert_not_called()

    def test_checker_child_traceback_reaches_the_release_runner_failure(self) -> None:
        driver = self.root / "failing_driver.py"
        stderr = (
            "FAIL: test_example\nTraceback:\nAssertionError: expected row\n"
            + "cleanup diagnostic\n" * 30
            + "FAILED (failures=1)\n"
        )
        driver.write_text(f"import sys\nsys.stderr.write({stderr!r})\nsys.exit(1)\n")
        checker = self.root / "checker.py"
        checker.write_text(
            "import os, sys\nfrom pathlib import Path\n"
            f"sys.path.insert(0, {str(ROOT / 'buildsys')!r})\n"
            "from check_python_wheel import run, WheelCheckError\n"
            "try:\n"
            f"    run([sys.executable, {str(driver)!r}], Path({str(self.root)!r}), "
            f"dict(os.environ), Path({str(self.root / 'integration')!r}))\n"
            "except WheelCheckError as error:\n"
            "    print(error, file=sys.stderr)\n    sys.exit(2)\n"
        )
        reports = self.root / "reports"
        reports.mkdir()
        with self.assertRaises(run_python_release.ReleaseRunError) as caught:
            run_python_release.run_stage(
                "check", (sys.executable, str(checker)), dict(os.environ), reports
            )
        message = str(caught.exception)
        self.assertIn("check failed with exit status 2", message)
        self.assertIn("FAIL: test_example", message)
        self.assertIn("AssertionError: expected row", message)
        self.assertIn("FAILED (failures=1)", message)
        self.assertEqual((self.root / "integration.stderr").read_text(), stderr)

    def test_output_cleanup_protects_source_inputs_and_symlink_targets(self) -> None:
        inputs = [self.args.static_library, self.args.resource_root]
        for output in (ROOT, ROOT.parent, self.kernel, self.args.resource_root / "output"):
            with self.subTest(output=output):
                with self.assertRaises(run_python_release.ReleaseRunError):
                    run_python_release.prepare_output(output, inputs)
        self.args.output_dir.mkdir()
        (self.args.output_dir / "raw").symlink_to(self.kernel, target_is_directory=True)
        with self.assertRaisesRegex(run_python_release.ReleaseRunError, "regular directory"):
            run_python_release.prepare_output(self.args.output_dir, inputs)
        self.assertTrue(self.args.static_library.is_file())
        (self.args.output_dir / "raw").unlink()
        protected = self.args.output_dir / "reports/input.json"
        protected.parent.mkdir()
        protected.write_text("kernel receipt")
        with self.assertRaisesRegex(run_python_release.ReleaseRunError, "overlaps a kernel input"):
            run_python_release.prepare_output(self.args.output_dir, [protected])
        self.assertEqual(protected.read_text(), "kernel receipt")

    def test_platform_cannot_weaken_the_release_baseline(self) -> None:
        self.args.platform = "manylinux_2_28_x86_64"
        with mock.patch.object(run_python_release, "run_stage") as stage:
            with self.assertRaisesRegex(run_python_release.ReleaseRunError,
                                        "platform must match.*manylinux_2_17_x86_64"):
                run_python_release.run_release(self.args)
        stage.assert_not_called()

    def test_child_start_error_identifies_the_stage_and_keeps_a_log(self) -> None:
        reports = self.root / "reports"
        reports.mkdir()
        with self.assertRaisesRegex(run_python_release.ReleaseRunError, "repair could not start"):
            run_python_release.run_stage("repair", [str(self.root / "missing-program")],
                                         dict(os.environ), reports)
        self.assertIn("missing-program", (reports / "repair.log").read_text())

    def test_make_entry_uses_prebuilt_inputs_and_cli_works_outside_the_checkout(self) -> None:
        result = subprocess.run(
            ["make", "--no-print-directory", "-s", "-n", "python-release-wheel-check",
             f"PYTHON={sys.executable}", f"PYTHON_KERNEL_STATIC_LIBRARY={self.args.static_library}",
             f"PYTHON_RELEASE_WHEEL_ROOT={self.args.output_dir}"],
            cwd=ROOT, check=True, text=True, capture_output=True,
        )
        command = shlex.split(result.stdout.replace("\\\n", ""))
        self.assertEqual(command[:2], [sys.executable, "buildsys/run_python_release.py"])
        self.assertEqual(option(command, "--static-library"), str(self.args.static_library))
        self.assertEqual(option(command, "--output-dir"), str(self.args.output_dir))
        self.assertEqual(command.count("--static-library"), 1)
        result = subprocess.run(
            [sys.executable, "-E", "-S", "-B", str(ROOT / "buildsys/run_python_release.py"), "--help"],
            cwd=self.root, check=True, text=True, capture_output=True,
        )
        self.assertIn("prebuilt kernel", result.stdout)

    @unittest.skipUnless(
        platform.system() == "Linux" and platform.machine() == "x86_64"
        and all(shutil.which(tool) for tool in ("cc", "auditwheel", "patchelf"))
        and importlib.util.find_spec("pip") is not None,
        "small native wheel handoff requires Linux x86_64, cc, auditwheel, patchelf, and pip",
    )
    def test_real_small_wheel_repair_and_pip_install_do_not_imply_product_acceptance(self) -> None:
        # Replace only kernel staging at the test boundary. Archive creation,
        # receipts, repair, pip installation, and product rejection are real.
        builder = self.root / "fixture_backend.py"
        builder.write_text(textwrap.dedent(f"""\
            import hashlib
            import os
            import subprocess
            import sys
            from pathlib import Path
            from unittest import mock
            sys.path.insert(0, {str(ROOT / 'python')!r})
            import build_backend

            def stage(root):
                package = root / "postgamma"
                package.mkdir()
                (package / "__init__.py").write_text(
                    "from ctypes import CDLL\\nfrom pathlib import Path\\n"
                    "def value():\\n"
                    "    return CDLL(str(Path(__file__).with_name('fixture.so'))).value()\\n"
                )
                subprocess.run(
                    ["cc", "-shared", "-fPIC", "-x", "c", "-", "-o", str(package / "fixture.so")],
                    input="int value(void) {{ return 42; }}\\n", text=True, check=True,
                )
                build_backend._stage_metadata(root / build_backend.DIST_INFO)

            library = Path(os.environ["POSTGAMMA_STATIC_LIBRARY"])
            digest = hashlib.sha256(library.read_bytes()).hexdigest()
            with mock.patch.object(build_backend, "_stage_wheel", stage), \\
                 mock.patch.object(build_backend, "_static_kernel", return_value=(library, [], digest)):
                raise SystemExit(build_backend.main())
            """), encoding="utf-8")
        real_stage = run_python_release.run_stage

        def stage(name, command, environment, reports):
            if name == "build":
                command = [command[0], str(builder), *command[2:]]
            real_stage(name, command, environment, reports)

        digests = []
        for supplied in (None, "946684800", "invalid"):
            with self.subTest(supplied=supplied):
                environment = dict(os.environ)
                environment.pop("SOURCE_DATE_EPOCH", None)
                if supplied is not None:
                    environment["SOURCE_DATE_EPOCH"] = supplied
                with mock.patch.dict(os.environ, environment, clear=True), \
                     mock.patch.object(run_python_release, "run_stage", side_effect=stage):
                    with self.assertRaisesRegex(run_python_release.ReleaseRunError,
                                                "(?s)check failed.*static-kernel marker is invalid"):
                        run_python_release.run_release(self.args)
                wheel = next((self.args.output_dir / "wheel").glob("*.whl"))
                digests.append(hashlib.sha256(wheel.read_bytes()).hexdigest())
                with zipfile.ZipFile(wheel) as archive:
                    self.assertTrue(all(info.date_time == (1980, 1, 1, 0, 0, 0)
                                        for info in archive.infolist()))
        self.assertEqual(len(set(digests)), 1)
        reports = self.args.output_dir / "reports"
        raw = json.loads((reports / "raw-wheel.json").read_text())
        repaired = json.loads((reports / "repaired-wheel.json").read_text())
        repair = json.loads((reports / "manylinux-repair.json").read_text())
        self.assertEqual(repair["source"]["sha256"], raw["sha256"])
        self.assertEqual(repair["repaired"]["sha256"], repaired["sha256"])
        self.assertEqual(raw["kernel_archive_sha256"], repaired["kernel_archive_sha256"])
        self.assertFalse((reports / "python-wheel.json").exists())
        wheel = self.args.output_dir / "wheel" / repaired["filename"]
        digest = hashlib.sha256(wheel.read_bytes()).hexdigest()
        self.assertEqual(digest, repaired["sha256"])
        install_args = argparse.Namespace(
            python=sys.executable, installer="pip", work_root=self.root / "fixture-check",
        )
        environment = check_python_wheel.sanitized_environment()
        site, result = check_python_wheel.install_wheel(
            install_args, wheel, self.root / "unused", environment,
        )
        self.assertEqual(result.returncode, 0)
        environment["PYTHONPATH"] = str(site)
        probe = subprocess.run(
            [sys.executable, "-c", "import postgamma; print(postgamma.value())"],
            cwd=self.root, env=environment, check=True, text=True, capture_output=True,
        )
        self.assertEqual(probe.stdout, "42\n")
        self.assertEqual(hashlib.sha256(wheel.read_bytes()).hexdigest(), digest)


if __name__ == "__main__":
    unittest.main()
