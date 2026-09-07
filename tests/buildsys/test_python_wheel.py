"""Tests for the complete Python wheel and release evidence gates."""

from __future__ import annotations

import hashlib
import email.parser
import json
import sys
import tempfile
import unittest
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(PROJECT_ROOT / "buildsys"))
sys.path.insert(0, str(PROJECT_ROOT / "python"))

import build_backend  # noqa: E402
import check_python_release_candidate  # noqa: E402
import check_python_release  # noqa: E402
import check_python_wheel  # noqa: E402


BASELINE_PATH = PROJECT_ROOT / "manifests/api/python-api-v1.json"
BASELINE = check_python_wheel.load_release_baseline(BASELINE_PATH)
BASELINE_SHA256 = hashlib.sha256(BASELINE_PATH.read_bytes()).hexdigest()


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
                "postgamma-0.1.0-cp312-cp312-manylinux_2_28_x86_64."
                "manylinux2014_x86_64.whl"
            ),
            [
                "cp312-cp312-manylinux2014_x86_64",
                "cp312-cp312-manylinux_2_28_x86_64",
            ],
        )

    def test_manylinux_policy_accepts_an_equal_or_older_glibc_floor(self) -> None:
        check_python_wheel.require_platform_policy(
            [
                "cp312-cp312-manylinux_2_28_x86_64",
                "cp312-cp312-manylinux2014_x86_64",
            ],
            "manylinux_2_28_x86_64",
        )
        with self.assertRaisesRegex(
            check_python_wheel.WheelCheckError, "exceed required policy"
        ):
            check_python_wheel.require_platform_policy(
                ["cp312-cp312-manylinux_2_34_x86_64"],
                "manylinux_2_28_x86_64",
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
            "tags": [f"{tag}-{tag}-manylinux_2_28_x86_64"],
            "platform_policy": "manylinux_2_28_x86_64",
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
            "policy": "manylinux_2_28_x86_64",
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
                "manylinux_2_28_x86_64",
                {**BASELINE, "python": {**BASELINE["python"], "tags": ["cp312"]}},
                BASELINE_SHA256,
            )
            self.assertEqual(report["wheel_count"], 1)
            self.assertTrue(report["project_license_declared"])
            self.assertTrue(report["public_release_ready"])
            self.assertFalse(report["publish_authorized"])

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
                    "manylinux_2_28_x86_64",
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
                    "manylinux_2_28_x86_64",
                    {
                        **BASELINE,
                        "python": {**BASELINE["python"], "tags": ["cp312"]},
                    },
                    BASELINE_SHA256,
                )


if __name__ == "__main__":
    unittest.main()
