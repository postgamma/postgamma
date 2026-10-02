"""Tests for the complete Python wheel and release evidence gates."""

from __future__ import annotations

import argparse
import csv
import email.parser
import hashlib
import io
import json
import os
import subprocess
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path
from unittest import mock


PROJECT_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(PROJECT_ROOT / "buildsys"))
sys.path.insert(0, str(PROJECT_ROOT / "python"))

import build_backend  # noqa: E402
import check_python_release_candidate  # noqa: E402
import check_python_release  # noqa: E402
import check_python_wheel  # noqa: E402
import repair_python_wheel  # noqa: E402


BASELINE_PATH = PROJECT_ROOT / "manifests/api/python-api-v1.json"
BASELINE = check_python_wheel.load_release_baseline(BASELINE_PATH)
BASELINE_SHA256 = hashlib.sha256(BASELINE_PATH.read_bytes()).hexdigest()


def write_zip_fixture(
    archive: zipfile.ZipFile, name: str, content: bytes | str
) -> None:
    archive.writestr(
        zipfile.ZipInfo(name, date_time=(1980, 1, 1, 0, 0, 0)), content
    )


def scenario_marker(
    *, passed: list[str] | None = None, skipped: list[str] | None = None
) -> str:
    document = {
        "schema_version": 1,
        "passed": sorted(
            check_python_wheel.EXPECTED_SCENARIOS if passed is None else passed
        ),
        "skipped": [] if skipped is None else skipped,
    }
    return (
        check_python_wheel.SCENARIO_MARKER
        + " "
        + json.dumps(document, sort_keys=True)
        + "\n"
    )


class PythonWheelTests(unittest.TestCase):
    def test_failed_child_reports_its_traceback_and_preserves_complete_logs(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            stdout = "first output\n" + "progress\n" * 100 + "last output\n"
            stderr = "FAIL: test_example\nTraceback:\nAssertionError: expected row\n"
            script = root / "driver.py"
            script.write_text(
                f"import sys\nsys.stdout.write({stdout!r})\n"
                f"sys.stderr.write({stderr!r})\nsys.exit(7)\n"
            )
            with self.assertRaises(check_python_wheel.WheelCheckError) as caught:
                check_python_wheel.run(
                    (sys.executable, str(script)), root, dict(os.environ),
                    root / "integration",
                )
            message = str(caught.exception)
            self.assertIn("exit status 7", message)
            self.assertIn("stdout tail:", message)
            self.assertIn("last output", message)
            self.assertNotIn("first output", message)
            self.assertIn(stderr.rstrip(), message)
            self.assertEqual((root / "integration.stdout").read_text(), stdout)
            self.assertEqual((root / "integration.stderr").read_text(), stderr)

    def test_failed_child_bounds_unbroken_log_lines_without_losing_full_output(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            script = root / "driver.py"
            script.write_text(
                "import sys\n"
                "sys.stdout.write('x' * 40000 + 'stdout-end')\n"
                "sys.stderr.write('y' * 40000 + 'stderr-end')\n"
                "sys.exit(1)\n"
            )
            with self.assertRaises(check_python_wheel.WheelCheckError) as caught:
                check_python_wheel.run(
                    (sys.executable, str(script)), root, dict(os.environ), root / "large"
                )
            message = str(caught.exception)
            self.assertLess(len(message), 18000)
            self.assertIn("stdout-end", message)
            self.assertIn("stderr-end", message)
            self.assertEqual(
                (root / "large.stdout").read_text(), "x" * 40000 + "stdout-end"
            )
            self.assertEqual(
                (root / "large.stderr").read_text(), "y" * 40000 + "stderr-end"
            )

    def test_license_files_are_checked_with_explicit_zip_directories(self) -> None:
        dist_info = build_backend.DIST_INFO
        tag = "cp312-cp312-linux_x86_64"
        wheel_name = f"postgamma-0.1.0a1-{tag}.whl"
        receipt = {"name": "postgamma", "version": "0.1.0a1", "tags": [tag]}

        def check_archive(*, change_license: bool = False) -> None:
            buffer = io.BytesIO()
            with zipfile.ZipFile(buffer, "w") as archive:
                write_zip_fixture(archive, f"{dist_info}/", b"")
                write_zip_fixture(archive, f"{dist_info}/licenses/", b"")
                write_zip_fixture(archive, f"{dist_info}/METADATA", build_backend._metadata())
                write_zip_fixture(
                    archive,
                    f"{dist_info}/WHEEL", f"Wheel-Version: 1.0\nTag: {tag}\n"
                )
                for name, path in build_backend.LICENSE_SOURCES:
                    content = path.read_bytes()
                    if change_license and name == "LICENSE":
                        content = b"changed license\n"
                    write_zip_fixture(archive, f"{dist_info}/licenses/{name}", content)
            with zipfile.ZipFile(buffer) as archive:
                members = {info.filename: info for info in archive.infolist()}
                check_python_wheel.validate_metadata(
                    archive,
                    members,
                    f"{dist_info}/RECORD",
                    receipt,
                    wheel_name,
                    BASELINE,
                )

        check_archive()
        with self.assertRaisesRegex(check_python_wheel.WheelCheckError, "license file changed"):
            check_archive(change_license=True)

    def test_record_tracks_files_after_repair_adds_zip_directories(self) -> None:
        record_name = "postgamma-0.1.0a1.dist-info/RECORD"
        files = {
            "postgamma/__init__.py": b"version = '0.1.0a1'\n",
            "postgamma-0.1.0a1.dist-info/licenses/LICENSE": b"license\n",
        }

        def check_archive(
            *, omit: str | None = None, wrong_digest: bool = False,
            record_directory: bool = False, directory_data: bytes = b"",
        ) -> str:
            buffer = io.BytesIO()
            with zipfile.ZipFile(buffer, "w") as archive:
                for name in (
                    "postgamma/",
                    "postgamma-0.1.0a1.dist-info/",
                    "postgamma-0.1.0a1.dist-info/licenses/",
                ):
                    write_zip_fixture(
                        archive, name, directory_data if name == "postgamma/" else b""
                    )
                for name, content in files.items():
                    write_zip_fixture(archive, name, content)
                rows = [
                    (
                        name,
                        check_python_wheel.digest(
                            b"incorrect" if wrong_digest else content
                        ),
                        str(len(content)),
                    )
                    for name, content in files.items()
                    if name != omit
                ]
                if record_directory:
                    rows.append(("postgamma/", "", ""))
                rows.append((record_name, "", ""))
                text = io.StringIO(newline="")
                csv.writer(text, lineterminator="\n").writerows(rows)
                write_zip_fixture(archive, record_name, text.getvalue())
            with zipfile.ZipFile(buffer) as archive:
                members = {info.filename: info for info in archive.infolist()}
                return check_python_wheel.validate_record(archive, members)

        self.assertEqual(check_archive(), record_name)
        with self.assertRaisesRegex(check_python_wheel.WheelCheckError, "omits 1 member"):
            check_archive(omit="postgamma/__init__.py")
        with self.assertRaisesRegex(check_python_wheel.WheelCheckError, "digest mismatch"):
            check_archive(wrong_digest=True)
        with self.assertRaisesRegex(check_python_wheel.WheelCheckError, "unknown or duplicate"):
            check_archive(record_directory=True)
        with self.assertRaisesRegex(check_python_wheel.WheelCheckError, "nonempty directory"):
            check_archive(directory_data=b"hidden data")

    def test_manylinux_repair_preserves_source_receipt_identity(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            raw = root / "raw"
            raw.mkdir()
            source_name = "postgamma-0.1.0a1-cp313-cp313-linux_x86_64.whl"
            source_wheel = raw / source_name
            source_wheel.write_bytes(b"native wheel")
            source_receipt = {
                "schema_version": 2,
                "kind": "postgamma.python-wheel-build",
                "name": BASELINE["release"]["name"],
                "version": BASELINE["release"]["version"],
                "filename": source_name,
                "sha256": hashlib.sha256(source_wheel.read_bytes()).hexdigest(),
                "size": source_wheel.stat().st_size,
                "tags": ["cp313-cp313-linux_x86_64"],
                "platform_policy": "linux_native",
                "kernel_linkage": "static",
                "bundled_kernel_library": False,
                "kernel_archive_sha256": "0" * 64,
                "python": "3.13.0",
                "metadata_version": BASELINE["wheel"]["metadata_version"],
                "requires_python": BASELINE["python"]["requires_python"],
                "project_url": BASELINE["wheel"]["project_urls"]["Homepage"],
                "license_files": sorted(
                    check_python_wheel.release_license_files(BASELINE)
                ),
                "extra_identity": {"input": "unchanged"},
            }
            receipt_path = root / "raw-wheel.json"
            receipt_path.write_text(json.dumps(source_receipt), encoding="utf-8")
            repaired_name = (
                "postgamma-0.1.0a1-cp313-cp313-manylinux_2_17_x86_64.whl"
            )
            tags = ["cp313-cp313-manylinux_2_17_x86_64"]

            def fake_auditwheel(arguments: tuple[str, ...]) -> subprocess.CompletedProcess[str]:
                if "repair" in arguments:
                    stage = Path(arguments[arguments.index("--wheel-dir") + 1])
                    (stage / repaired_name).write_bytes(b"repaired wheel")
                return subprocess.CompletedProcess(arguments, 0, "auditwheel test", "")

            with mock.patch.object(
                repair_python_wheel, "run", side_effect=fake_auditwheel
            ), mock.patch.object(repair_python_wheel, "wheel_tags", return_value=tags):
                receipt, _report = repair_python_wheel.repair(
                    argparse.Namespace(
                        receipt=receipt_path,
                        wheel_dir=raw,
                        output_dir=root / "repaired",
                        platform="manylinux_2_17_x86_64",
                        auditwheel="auditwheel",
                    )
                )

            changed = {"filename", "sha256", "size", "tags", "platform_policy"}
            self.assertEqual(set(receipt), set(source_receipt))
            for field in set(source_receipt) - changed:
                self.assertEqual(receipt[field], source_receipt[field], field)
            self.assertEqual(receipt["filename"], repaired_name)
            self.assertEqual(
                receipt["sha256"], hashlib.sha256(b"repaired wheel").hexdigest()
            )
            self.assertEqual(receipt["size"], len(b"repaired wheel"))
            self.assertEqual(receipt["platform_policy"], "manylinux_2_17_x86_64")
            self.assertEqual(receipt["tags"], tags)

    def test_native_extension_honors_the_release_compiler_baseline(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            package = root / "postgamma"
            package.mkdir()
            library = root / "libpostgamma.a"
            library.write_bytes(b"archive")
            completed = mock.Mock(returncode=0, stdout="", stderr="")
            environment = {
                "CC": "release-cc --driver-mode=gcc",
                "CFLAGS": "-march=x86-64 -mtune=generic",
            }
            with mock.patch.dict(os.environ, environment, clear=False), mock.patch(
                "build_backend.subprocess.run", return_value=completed
            ) as run:
                build_backend._compile_extension(package, library, ["-lm"])
            command = run.call_args.args[0]
            self.assertEqual(command[:2], ["release-cc", "--driver-mode=gcc"])
            self.assertIn("-march=x86-64", command)
            self.assertIn("-mtune=generic", command)
            self.assertIn(
                f"-Wl,--version-script={build_backend.NATIVE_VERSION_SCRIPT}",
                command,
            )

    def test_work_root_cleanup_removes_only_checker_owned_artifacts(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for name in (
                "audit-site",
                "installed-site",
                "execution",
                "examples",
                "integration",
                "pip-install",
                "site",
                "unit",
            ):
                path = root / name
                path.mkdir()
                (path / "stale").write_text("stale\n", encoding="utf-8")
            (root / "unowned").mkdir()

            self.assertEqual(check_python_wheel.prepare_work_root(root), root.resolve())

            self.assertTrue((root / "unowned").is_dir())
            for name in (
                "audit-site",
                "installed-site",
                "execution",
                "examples",
                "integration",
                "pip-install",
                "site",
                "unit",
            ):
                self.assertFalse((root / name).exists())

    def test_scenario_marker_derives_every_claimed_concurrency_gate(self) -> None:
        evidence = check_python_wheel.parse_scenario_marker(scenario_marker())
        self.assertEqual(
            evidence["gil_parallelism"],
            ["test_04_connections_execute_in_parallel_without_the_gil"],
        )
        self.assertEqual(
            evidence["timeout_and_cancel"],
            [
                "test_05_timeout_cancels_and_connection_is_reusable",
                "test_06_cross_thread_cancel",
            ],
        )

    def test_scenario_marker_rejects_a_renamed_or_missing_test(self) -> None:
        passed = sorted(check_python_wheel.EXPECTED_SCENARIOS)
        passed.remove("test_04_connections_execute_in_parallel_without_the_gil")
        passed.append("test_04_parallel_query")
        with self.assertRaisesRegex(
            check_python_wheel.WheelCheckError, "execution is incomplete"
        ):
            check_python_wheel.parse_scenario_marker(scenario_marker(passed=passed))

    def test_scenario_marker_rejects_a_skipped_required_test(self) -> None:
        with self.assertRaisesRegex(
            check_python_wheel.WheelCheckError, "were skipped"
        ):
            check_python_wheel.parse_scenario_marker(
                scenario_marker(
                    skipped=[
                        "test_07_inherited_handles_fail_closed_after_fork"
                    ]
                )
            )

    def test_documentation_examples_execute_and_match_stdout(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            examples = root / "examples/python"
            manifest = root / "docs/reference"
            examples.mkdir(parents=True)
            manifest.mkdir(parents=True)
            (examples / "hello.py").write_text(
                "print('hello')\n", encoding="utf-8"
            )
            (manifest / "python-examples.json").write_text(
                json.dumps(
                    {
                        "schema_version": 1,
                        "kind": "postgamma.python-documentation-examples",
                        "examples": [
                            {
                                "name": "hello",
                                "path": "hello.py",
                                "stdout": "hello\n",
                            }
                        ],
                    }
                ),
                encoding="utf-8",
            )
            evidence = check_python_wheel.execute_documentation_examples(
                python=sys.executable,
                project_root=root,
                work_root=root / "work",
                environment=check_python_wheel.sanitized_environment(),
                expected_names=["hello"],
            )
            self.assertEqual(evidence["names"], ["hello"])
            self.assertEqual(set(evidence["stdout_sha256"]), {"hello"})

    def test_compressed_wheel_filename_expands_to_metadata_tags(self) -> None:
        self.assertEqual(
            check_python_wheel.filename_tags(
                "postgamma-0.1.0-cp312-cp312-manylinux_2_17_x86_64."
                "manylinux2014_x86_64.whl"
            ),
            [
                "cp312-cp312-manylinux2014_x86_64",
                "cp312-cp312-manylinux_2_17_x86_64",
            ],
        )

    def test_manylinux_policy_accepts_an_equal_or_older_glibc_floor(self) -> None:
        check_python_wheel.require_platform_policy(
            [
                "cp312-cp312-manylinux_2_17_x86_64",
                "cp312-cp312-manylinux2010_x86_64",
            ],
            "manylinux_2_17_x86_64",
        )
        with self.assertRaisesRegex(
            check_python_wheel.WheelCheckError, "exceed required policy"
        ):
            check_python_wheel.require_platform_policy(
                ["cp312-cp312-manylinux_2_28_x86_64"],
                "manylinux_2_17_x86_64",
            )

    def test_manylinux_dependency_closure_accepts_system_zlib(self) -> None:
        dynamic = {"needed": ["libc.so.6", "libz.so.1"]}
        for label in ("Python extension", "PostgreSQL executable"):
            with self.subTest(binary=label):
                check_python_wheel.require_dependency_closure(
                    dynamic,
                    check_python_wheel.MANYLINUX_PLATFORM_DEPENDENCIES,
                    set(),
                    label,
                )

    def test_manylinux_dependency_closure_rejects_unbundled_libraries(self) -> None:
        for name in ("libpq.so.5", "libssl.so.3", "libprivate.so.1"):
            with self.subTest(dependency=name):
                with self.assertRaises(check_python_wheel.WheelCheckError) as caught:
                    check_python_wheel.require_dependency_closure(
                        {"needed": ["libc.so.6", "libz.so.1", name]},
                        check_python_wheel.MANYLINUX_PLATFORM_DEPENDENCIES,
                        set(),
                        "Python extension",
                    )
                self.assertEqual(
                    str(caught.exception),
                    f"Python extension has unbundled dependencies: {name}",
                )

    def test_manylinux_dependency_closure_requires_the_exact_bundled_name(self) -> None:
        name = "libprivate-12345678.so.1"
        dynamic = {"needed": ["libc.so.6", "libz.so.1", name]}
        check_python_wheel.require_dependency_closure(
            dynamic,
            check_python_wheel.MANYLINUX_PLATFORM_DEPENDENCIES,
            {name},
            "Python extension",
        )
        with self.assertRaises(check_python_wheel.WheelCheckError) as caught:
            check_python_wheel.require_dependency_closure(
                dynamic,
                check_python_wheel.MANYLINUX_PLATFORM_DEPENDENCIES,
                {"libprivate.so.1"},
                "Python extension",
            )
        self.assertEqual(
            str(caught.exception),
            f"Python extension has unbundled dependencies: {name}",
        )

    def test_python_source_copy_is_recursive_and_excludes_build_inputs(self) -> None:
        original = build_backend.PACKAGE_SOURCE
        try:
            with tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                source = root / "source"
                target = root / "target"
                (source / "adapter").mkdir(parents=True)
                (source / "adapter/__init__.py").write_text("VALUE = 1\n")
                (source / "_native.c").write_text("ignored\n")
                (source / "_native_requests.c").write_text("ignored\n")
                (source / "_native_internal.h").write_text("ignored\n")
                (source / "__pycache__").mkdir()
                (source / "__pycache__/ignored.pyc").write_bytes(b"ignored")
                build_backend.PACKAGE_SOURCE = source
                build_backend._copy_python_sources(target)
                self.assertTrue((target / "adapter/__init__.py").is_file())
                self.assertFalse((target / "_native.c").exists())
                self.assertFalse((target / "_native_requests.c").exists())
                self.assertFalse((target / "_native_internal.h").exists())
                self.assertFalse((target / "__pycache__").exists())
        finally:
            build_backend.PACKAGE_SOURCE = original

    def test_backend_has_one_metadata_source_and_no_sdist_hook(self) -> None:
        pyproject = (PROJECT_ROOT / "python/pyproject.toml").read_text(encoding="utf-8")
        self.assertNotIn("[project]", pyproject)
        self.assertFalse(hasattr(build_backend, "build_sdist"))
        self.assertEqual(
            build_backend.KERNEL_ARCHIVE_NAME,
            check_python_wheel.KERNEL_ARCHIVE_NAME,
        )
        self.assertEqual(BASELINE["wheel"]["kernel_linkage"], "static")
        self.assertFalse(BASELINE["wheel"]["bundled_kernel_library"])
        self.assertEqual(
            "postgamma/_resources/lib/" + build_backend.STATIC_KERNEL_MARKER,
            check_python_wheel.STATIC_KERNEL_MARKER,
        )

    def test_wheel_metadata_has_description_and_complete_licenses(self) -> None:
        metadata = email.parser.BytesParser().parsebytes(build_backend._metadata())
        self.assertEqual(metadata["Metadata-Version"], "2.4")
        self.assertEqual(metadata["Version"], BASELINE["release"]["version"])
        self.assertEqual(
            metadata.get_all("License-File"),
            [
                "LICENSE",
                "NOTICE",
                "THIRD_PARTY_NOTICES",
                "LICENSE.postgresql",
                "LICENSE.pgvector",
            ],
        )
        self.assertEqual(metadata["License-Expression"], "Apache-2.0")
        self.assertEqual(metadata["Author"], "PostGamma contributors")
        self.assertEqual(
            metadata.get_all("Project-URL"),
            ["Homepage, https://postgamma.com"],
        )
        self.assertIn("PostGamma embeds PostgreSQL 19", metadata.get_payload())
        with tempfile.TemporaryDirectory() as temporary:
            target = Path(temporary) / build_backend.DIST_INFO
            build_backend._stage_metadata(target)
            for name, expected in check_python_wheel.release_license_files(
                BASELINE
            ).items():
                license_path = target / "licenses" / name
                self.assertEqual(
                    hashlib.sha256(license_path.read_bytes()).hexdigest(),
                    expected,
                )

        self.assertEqual(
            build_backend._receipt_license_files(),
            sorted(check_python_wheel.release_license_files(BASELINE)),
        )

    def test_native_extension_source_partition_is_complete(self) -> None:
        on_disk = {
            path.name
            for path in build_backend.PACKAGE_SOURCE.glob("_native*.c")
        }
        self.assertEqual(set(build_backend.NATIVE_SOURCES), on_disk)

    def test_python_source_surface_matches_the_frozen_baseline(self) -> None:
        source = check_python_release_candidate.validate_source(PROJECT_ROOT, BASELINE)
        self.assertEqual(source["version"], "0.1.0a1")
        self.assertEqual(source["public_export_count"], 74)

    def test_python_license_inventory_is_order_independent_but_exact(self) -> None:
        expected = [
            "LICENSE",
            "NOTICE",
            "THIRD_PARTY_NOTICES",
            "LICENSE.pgvector",
            "LICENSE.postgresql",
        ]
        self.assertTrue(
            check_python_release_candidate.same_unique_strings(
                [
                    "LICENSE.postgresql",
                    "NOTICE",
                    "LICENSE",
                    "THIRD_PARTY_NOTICES",
                    "LICENSE.pgvector",
                ],
                expected,
            )
        )
        self.assertFalse(
            check_python_release_candidate.same_unique_strings(
                ["LICENSE", "LICENSE.pgvector", "LICENSE.pgvector"],
                expected,
            )
        )


class PythonReleaseTests(unittest.TestCase):
    def write_entry(self, root: Path, tag: str = "cp312") -> Path:
        entry = root / tag
        wheel = entry / "wheel/postgamma.whl"
        wheel.parent.mkdir(parents=True)
        wheel.write_bytes(b"wheel")
        digest = hashlib.sha256(b"wheel").hexdigest()
        receipt = {
            "schema_version": 2,
            "kind": "postgamma.python-wheel-build",
            "name": "postgamma",
            "version": BASELINE["release"]["version"],
            "filename": wheel.name,
            "sha256": digest,
            "size": 5,
            "tags": [f"{tag}-{tag}-manylinux_2_17_x86_64"],
            "platform_policy": "manylinux_2_17_x86_64",
            "kernel_linkage": "static",
            "bundled_kernel_library": False,
            "kernel_archive_sha256": "0" * 64,
            "python": "3.12.7",
            "metadata_version": BASELINE["wheel"]["metadata_version"],
            "requires_python": BASELINE["python"]["requires_python"],
            "project_url": BASELINE["wheel"]["project_urls"]["Homepage"],
            "license_files": sorted(
                check_python_wheel.release_license_files(BASELINE)
            ),
        }
        gates: dict[str, object] = {
            name: {} for name in check_python_release.REQUIRED_GATE_NAMES
        }
        gates["installation"] = {"mode": "pip", "pip_returncode": 0}
        gates["public_api"] = {
            "export_count": len(BASELINE["public_exports"]),
            "baseline_sha256": BASELINE_SHA256,
        }
        gates["documentation_examples"] = {
            "count": len(BASELINE["documentation"]["examples"]),
            "names": BASELINE["documentation"]["examples"],
            "stdout_sha256": {
                name: "0" * 64
                for name in BASELINE["documentation"]["examples"]
            },
        }
        for scenario, gate in check_python_wheel.EXPECTED_SCENARIOS.items():
            assert isinstance(gates[gate], dict)
            gates[gate].setdefault("scenarios", []).append(scenario)
        evidence = {
            "schema_version": 2,
            "kind": "postgamma.python-wheel-evidence",
            "status": "pass",
            "wheel": {"sha256": digest, "member_count": 670},
            "kernel": {
                "linkage": "static",
                "separate_library": False,
                "postgamma_dynamic_dependencies": 0,
                "native_dynamic_exports": ["PyInit__native"],
            },
            "gates": gates,
            "release_baseline": {"sha256": BASELINE_SHA256},
        }
        repair = {
            "schema_version": 1,
            "kind": "postgamma.python-manylinux-repair",
            "status": "pass",
            "policy": "manylinux_2_17_x86_64",
            "repaired": {"sha256": digest},
        }
        reports = entry / "reports"
        reports.mkdir()
        for name, document in (
            ("repaired-wheel.json", receipt),
            ("python-wheel.json", evidence),
            ("manylinux-repair.json", repair),
        ):
            (reports / name).write_text(json.dumps(document), encoding="utf-8")
        return entry

    def test_release_matrix_requires_a_clean_installed_measured_wheel(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            self.write_entry(root)
            report = check_python_release.collect(
                root,
                ["cp312"],
                "manylinux_2_17_x86_64",
                {**BASELINE, "python": {**BASELINE["python"], "tags": ["cp312"]}},
                BASELINE_SHA256,
            )
            self.assertEqual(report["wheel_count"], 1)
            self.assertTrue(report["project_license_declared"])
            self.assertTrue(report["public_release_ready"])
            self.assertFalse(report["publish_authorized"])

    def test_release_matrix_rejects_missing_or_changed_receipt_identity(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            entry = self.write_entry(root)
            path = entry / "reports/repaired-wheel.json"
            receipt = json.loads(path.read_text(encoding="utf-8"))
            for field, value, message in (
                ("project_url", None, "missing required field.*project_url"),
                ("tags", None, "missing required field.*tags"),
                ("name", "another", "name differs"),
                ("version", "0.0.0", "version differs"),
                ("metadata_version", "2.3", "metadata_version differs"),
                ("requires_python", ">=3.12", "requires_python differs"),
                ("project_url", "https://invalid.example", "project_url differs"),
                ("license_files", receipt["license_files"][:-1], "license_files differs"),
                ("kernel_archive_sha256", "invalid", "kernel_archive_sha256"),
                ("platform_policy", "linux_native", "platform_policy"),
                ("tags", ["cp310-cp310-manylinux_2_17_x86_64"], "wheel tags"),
                ("python", "3.10.0", "interpreter version"),
            ):
                with self.subTest(field=field, value=value):
                    changed = {**receipt, field: value}
                    if value is None:
                        del changed[field]
                    path.write_text(json.dumps(changed), encoding="utf-8")
                    with self.assertRaisesRegex(check_python_wheel.WheelCheckError, message):
                        check_python_release.validate_entry(
                            root, "cp312", "manylinux_2_17_x86_64",
                            BASELINE, BASELINE_SHA256,
                        )

    def test_release_matrix_rechecks_actual_wheel_identity(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            entry = self.write_entry(root)
            path = entry / "reports/repaired-wheel.json"
            receipt = json.loads(path.read_text(encoding="utf-8"))
            for field, value in (
                ("filename", "missing.whl"), ("sha256", "0" * 64),
                ("size", receipt["size"] + 1),
            ):
                with self.subTest(field=field):
                    path.write_text(json.dumps({**receipt, field: value}), encoding="utf-8")
                    with self.assertRaisesRegex(
                        check_python_wheel.WheelCheckError, "wheel does not match its receipt"
                    ):
                        check_python_release.validate_entry(
                            root, "cp312", "manylinux_2_17_x86_64",
                            BASELINE, BASELINE_SHA256,
                        )
            path.write_text(json.dumps(receipt), encoding="utf-8")
            (entry / "wheel" / receipt["filename"]).write_bytes(b"changed wheel")
            with self.assertRaisesRegex(
                check_python_wheel.WheelCheckError, "wheel does not match its receipt"
            ):
                check_python_release.validate_entry(
                    root, "cp312", "manylinux_2_17_x86_64", BASELINE, BASELINE_SHA256
                )

    def test_release_matrix_keeps_repair_and_execution_evidence_bound_to_bytes(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            entry = self.write_entry(root)
            path = entry / "reports/repaired-wheel.json"
            receipt = json.loads(path.read_text(encoding="utf-8"))
            wheel = entry / "wheel" / receipt["filename"]
            wheel.write_bytes(b"changed wheel")
            receipt["sha256"] = hashlib.sha256(wheel.read_bytes()).hexdigest()
            receipt["size"] = wheel.stat().st_size
            path.write_text(json.dumps(receipt), encoding="utf-8")
            with self.assertRaisesRegex(check_python_wheel.WheelCheckError, "repair evidence"):
                check_python_release.validate_entry(
                    root, "cp312", "manylinux_2_17_x86_64", BASELINE, BASELINE_SHA256
                )
            repair_path = entry / "reports/manylinux-repair.json"
            repair = json.loads(repair_path.read_text(encoding="utf-8"))
            repair["repaired"]["sha256"] = receipt["sha256"]
            repair_path.write_text(json.dumps(repair), encoding="utf-8")
            with self.assertRaisesRegex(check_python_wheel.WheelCheckError, "execution evidence"):
                check_python_release.validate_entry(
                    root, "cp312", "manylinux_2_17_x86_64", BASELINE, BASELINE_SHA256
                )

    def test_release_matrix_rejects_archive_only_testing(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            entry = self.write_entry(root)
            evidence_path = entry / "reports/python-wheel.json"
            evidence = json.loads(evidence_path.read_text(encoding="utf-8"))
            evidence["gates"]["installation"] = {
                "mode": "archive",
                "pip_returncode": None,
            }
            evidence_path.write_text(json.dumps(evidence), encoding="utf-8")
            with self.assertRaisesRegex(
                check_python_wheel.WheelCheckError, "clean pip install"
            ):
                check_python_release.collect(
                    root,
                    ["cp312"],
                    "manylinux_2_17_x86_64",
                    {
                        **BASELINE,
                        "python": {**BASELINE["python"], "tags": ["cp312"]},
                    },
                    BASELINE_SHA256,
                )

    def test_release_matrix_rejects_malformed_documentation_evidence(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            entry = self.write_entry(root)
            evidence_path = entry / "reports/python-wheel.json"
            evidence = json.loads(evidence_path.read_text(encoding="utf-8"))
            evidence["gates"]["documentation_examples"]["stdout_sha256"] = None
            evidence_path.write_text(json.dumps(evidence), encoding="utf-8")
            with self.assertRaisesRegex(
                check_python_wheel.WheelCheckError,
                "documentation example execution evidence",
            ):
                check_python_release.collect(
                    root,
                    ["cp312"],
                    "manylinux_2_17_x86_64",
                    {
                        **BASELINE,
                        "python": {**BASELINE["python"], "tags": ["cp312"]},
                    },
                    BASELINE_SHA256,
                )


if __name__ == "__main__":
    unittest.main()
