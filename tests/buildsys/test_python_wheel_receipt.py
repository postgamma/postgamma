"""Receipt generation and backend entry-point compatibility tests."""

from __future__ import annotations

import argparse
import copy
import hashlib
import io
import json
import subprocess
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path
from typing import Any
from unittest import mock


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "python"))

import build_backend  # noqa: E402
import check_python_wheel  # noqa: E402
import repair_python_wheel  # noqa: E402
from python_wheel_receipt import (  # noqa: E402
    WheelReceiptError,
    build_receipt,
    receipt_for_repaired_wheel,
    validate_receipt,
)


class WheelReceiptTests(unittest.TestCase):
    def setUp(self) -> None:
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.wheel = self.root / "postgamma-0.1.0a1-cp312-cp312-linux_x86_64.whl"
        with zipfile.ZipFile(self.wheel, "w") as archive:
            archive.writestr(
                zipfile.ZipInfo("postgamma/__init__.py", (1980, 1, 1, 0, 0, 0)),
                b"# Receipt test fixture.\n",
            )
        self.identity = {
            "name": "postgamma",
            "version": "0.1.0a1",
            "tags": ["cp312-cp312-linux_x86_64"],
            "platform_policy": "linux_native",
            "kernel_archive_sha256": "a" * 64,
            "python": "3.12.0",
            "metadata_version": "2.4",
            "requires_python": ">=3.10",
            "project_url": "https://postgamma.com",
            "license_files": [
                "LICENSE",
                "NOTICE",
                "THIRD_PARTY_NOTICES",
                "LICENSE.postgresql",
                "LICENSE.pgvector",
            ],
        }

    def test_generation_preserves_schema_and_binds_actual_wheel_bytes(self) -> None:
        content = self.wheel.read_bytes()
        receipt = build_receipt(self.wheel, **self.identity)
        self.assertEqual(
            receipt,
            {
                "schema_version": 2,
                "kind": "postgamma.python-wheel-build",
                **self.identity,
                "filename": self.wheel.name,
                "sha256": hashlib.sha256(content).hexdigest(),
                "size": len(content),
                "kernel_linkage": "static",
                "bundled_kernel_library": False,
                "license_files": sorted(self.identity["license_files"]),
            },
        )
        self.assertEqual(self.wheel.read_bytes(), content)
        self.assertEqual(json.loads(json.dumps(receipt)), receipt)
        self.assertEqual(
            self.identity["license_files"][1], "NOTICE", "input order was changed"
        )
        self.identity["tags"].append("cp312-cp312-other")
        self.identity["license_files"].append("EXTRA")
        self.assertEqual(receipt["tags"], ["cp312-cp312-linux_x86_64"])
        self.assertNotIn("EXTRA", receipt["license_files"])

    def test_missing_fields_are_named_in_diagnostics(self) -> None:
        receipt = build_receipt(self.wheel, **self.identity)
        for name in receipt:
            with self.subTest(field=name):
                incomplete = dict(receipt)
                del incomplete[name]
                with self.assertRaisesRegex(
                    WheelReceiptError, f"missing required field\\(s\\): {name}$"
                ):
                    validate_receipt(incomplete)
        with self.assertRaisesRegex(WheelReceiptError, "JSON object"):
            validate_receipt([])

    def test_repair_changes_only_artifact_fields_and_preserves_source_identity(self) -> None:
        source = build_receipt(self.wheel, **self.identity)
        source["extra_identity"] = {"inputs": ["kernel"]}
        original = copy.deepcopy(source)
        repaired_wheel = self.root / self.wheel.name.replace(
            "linux_x86_64", "manylinux_2_17_x86_64"
        )
        repaired_wheel.write_bytes(self.wheel.read_bytes() + b"repaired")
        tags = ["cp312-cp312-manylinux_2_17_x86_64"]
        receipt = receipt_for_repaired_wheel(
            source,
            repaired_wheel,
            tags=tags,
            platform_policy="manylinux_2_17_x86_64",
        )
        self.assertEqual(
            {name for name in source if receipt[name] != source[name]},
            {"filename", "sha256", "size", "tags", "platform_policy"},
        )
        self.assertEqual(receipt["filename"], repaired_wheel.name)
        self.assertEqual(
            receipt["sha256"], hashlib.sha256(repaired_wheel.read_bytes()).hexdigest()
        )
        self.assertEqual(receipt["size"], repaired_wheel.stat().st_size)
        self.assertEqual(receipt["tags"], tags)
        self.assertEqual(receipt["platform_policy"], "manylinux_2_17_x86_64")
        self.assertEqual(source, original)
        tags.append("cp312-cp312-other")
        receipt["license_files"].append("EXTRA")
        receipt["extra_identity"]["inputs"].append("another")
        self.assertEqual(source, original)
        self.assertEqual(receipt["tags"], ["cp312-cp312-manylinux_2_17_x86_64"])

    def test_repair_rejects_incomplete_source_and_invalid_new_fields(self) -> None:
        source = build_receipt(self.wheel, **self.identity)
        incomplete = {key: value for key, value in source.items() if key != "project_url"}
        for document, tags, policy, message in (
            (incomplete, ["tag"], "policy", "missing required field.*project_url"),
            (source, [], "policy", "tags"),
            (source, ["tag"], "", "platform_policy"),
        ):
            with self.subTest(message=message):
                with self.assertRaisesRegex(WheelReceiptError, message):
                    receipt_for_repaired_wheel(
                        document, self.wheel, tags=tags, platform_policy=policy
                    )

    def test_repair_refuses_incomplete_source_before_running_auditwheel(self) -> None:
        incomplete = build_receipt(self.wheel, **self.identity)
        del incomplete["project_url"]
        path = self.root / "receipt.json"
        path.write_text(json.dumps(incomplete), encoding="utf-8")
        with mock.patch.object(repair_python_wheel, "run") as auditwheel:
            with self.assertRaisesRegex(
                check_python_wheel.WheelCheckError, "missing required field.*project_url"
            ):
                repair_python_wheel.repair(
                    argparse.Namespace(receipt=path, wheel_dir=self.root)
                )
            auditwheel.assert_not_called()

    def test_repair_rechecks_source_artifact_and_platform(self) -> None:
        receipt = build_receipt(self.wheel, **self.identity)
        self.assertEqual(
            repair_python_wheel.validate_source_receipt(receipt, self.root), self.wheel
        )
        for field, value, message in (
            ("filename", "missing.whl", "native wheel is missing"),
            ("sha256", "0" * 64, "no longer matches"),
            ("size", receipt["size"] + 1, "no longer matches"),
            ("platform_policy", "manylinux_2_17_x86_64", "platform_policy"),
        ):
            with self.subTest(changed=field):
                with self.assertRaisesRegex(check_python_wheel.WheelCheckError, message):
                    repair_python_wheel.validate_source_receipt(
                        {**receipt, field: value}, self.root
                    )
        self.wheel.write_bytes(b"different wheel")
        with self.assertRaisesRegex(check_python_wheel.WheelCheckError, "no longer matches"):
            repair_python_wheel.validate_source_receipt(receipt, self.root)

    def test_builder_does_not_default_missing_identity(self) -> None:
        for name in self.identity:
            with self.subTest(field=name):
                incomplete = dict(self.identity)
                del incomplete[name]
                with self.assertRaisesRegex(TypeError, f"missing.*'{name}'"):
                    build_receipt(self.wheel, **incomplete)

    def test_malformed_fields_are_rejected_without_type_coercion(self) -> None:
        receipt = build_receipt(self.wheel, **self.identity)
        invalid = {
            "schema_version": [1, 2.0, "2"],
            "kind": ["other", None],
            "kernel_linkage": ["shared"],
            "bundled_kernel_library": [True, 0],
            "sha256": ["A" * 64, "0" * 63, 0],
            "kernel_archive_sha256": ["g" * 64, ""],
            "size": [-1, 0, True, 1.5, "1"],
            "name": [None, ""],
            "version": [None, " "],
            "filename": [None, ""],
            "platform_policy": [None, ""],
            "python": [None, ""],
            "metadata_version": [None, ""],
            "requires_python": [None, ""],
            "project_url": [None, " "],
            "tags": [[], "cp312-cp312-linux_x86_64", ["tag", "tag"], [None]],
            "license_files": [[], ["LICENSE", "LICENSE"], [""], [["LICENSE"]]],
        }
        for name, values in invalid.items():
            for value in values:
                with self.subTest(field=name, value=value):
                    with self.assertRaisesRegex(WheelReceiptError, name):
                        validate_receipt({**receipt, name: value})
        for name, value in (("project_url", ""), ("license_files", [None])):
            with self.subTest(builder_field=name):
                with self.assertRaisesRegex(WheelReceiptError, name):
                    build_receipt(self.wheel, **{**self.identity, name: value})

    def test_backend_writes_the_existing_receipt_format(self) -> None:
        destination = self.root / "wheel"
        receipt_path = self.root / "reports/wheel.json"
        tag = "cp312-cp312-linux_x86_64"

        def stage_fixture(stage: Path) -> None:
            package = stage / "postgamma"
            package.mkdir()
            (package / "__init__.py").write_bytes(b"# Receipt test fixture.\n")
            build_backend._stage_metadata(stage / build_backend.DIST_INFO)

        # Exercise real archive creation and receipt writing without a kernel build.
        with mock.patch.object(
            build_backend, "_stage_wheel", side_effect=stage_fixture
        ), mock.patch.object(
            build_backend, "_static_kernel", return_value=(None, [], "a" * 64)
        ), mock.patch.object(
            build_backend, "_wheel_tag", return_value=tag
        ), mock.patch.object(
            sys,
            "argv",
            [
                "build_backend.py", "--wheel-dir", str(destination),
                "--receipt", str(receipt_path),
            ],
        ), mock.patch("sys.stdout", new_callable=io.StringIO) as output:
            self.assertEqual(build_backend.main(), 0)

        wheel = destination / f"postgamma-{build_backend.VERSION}-{tag}.whl"
        content = wheel.read_bytes()
        expected = {
            "schema_version": 2,
            "kind": "postgamma.python-wheel-build",
            "name": "postgamma",
            "version": build_backend.VERSION,
            "filename": wheel.name,
            "sha256": hashlib.sha256(content).hexdigest(),
            "size": len(content),
            "tags": [tag],
            "platform_policy": "linux_native",
            "kernel_linkage": "static",
            "bundled_kernel_library": False,
            "kernel_archive_sha256": "a" * 64,
            "python": sys.version.split()[0],
            "metadata_version": "2.4",
            "requires_python": ">=3.10",
            "project_url": "https://postgamma.com",
            "license_files": [
                "LICENSE", "LICENSE.pgvector", "LICENSE.postgresql",
                "NOTICE", "THIRD_PARTY_NOTICES",
            ],
        }
        self.assertEqual(
            receipt_path.read_text(encoding="utf-8"),
            json.dumps(expected, indent=2, sort_keys=True) + "\n",
        )
        self.assertEqual(output.getvalue(), str(wheel) + "\n")
        with zipfile.ZipFile(wheel) as archive:
            self.assertIn(
                "Project-URL: Homepage, " + expected["project_url"],
                archive.read(f"{build_backend.DIST_INFO}/METADATA").decode(),
            )
            self.assertEqual(
                sorted(
                    name.rsplit("/", 1)[-1]
                    for name in archive.namelist()
                    if "/licenses/" in name
                ),
                expected["license_files"],
            )

    def test_backend_import_and_cli_do_not_require_the_checkout_cwd_or_site(self) -> None:
        metadata = self.root / "metadata"
        script = (
            "import sys; from pathlib import Path; "
            "sys.path.insert(0, sys.argv[1]); import build_backend; "
            "assert build_backend.get_requires_for_build_wheel() == []; "
            "result = build_backend.prepare_metadata_for_build_wheel(sys.argv[2]); "
            "assert (Path(sys.argv[2]) / result / 'METADATA').is_file()"
        )
        for arguments in (
            ["-c", script, str(ROOT / "python"), str(metadata)],
            [str(ROOT / "python/build_backend.py"), "--help"],
        ):
            with self.subTest(entry=arguments[0]):
                result = subprocess.run(
                    [sys.executable, "-I", "-S", "-B", *arguments],
                    cwd=self.root,
                    check=False,
                    capture_output=True,
                    text=True,
                    timeout=20,
                )
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


class WheelReceiptConsumerTests(unittest.TestCase):
    """Exercise the real checker through metadata validation, without a kernel."""

    def setUp(self) -> None:
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.stage = self.root / "stage"
        self.dist_info = self.stage / build_backend.DIST_INFO
        self.tag = "cp312-cp312-manylinux_2_17_x86_64"
        with mock.patch.object(build_backend, "_wheel_tag", return_value=self.tag):
            build_backend._stage_metadata(self.dist_info)
        wheel_dir = self.root / "wheel"
        wheel_dir.mkdir()
        self.wheel = wheel_dir / f"postgamma-{build_backend.VERSION}-{self.tag}.whl"
        self.receipt_path = self.root / "receipt.json"
        self.args = argparse.Namespace(
            baseline=ROOT / "manifests/api/python-api-v1.json",
            receipt=self.receipt_path,
            wheel_dir=wheel_dir,
            work_root=self.root / "work",
            forbidden_prefix=[],
            require_platform="manylinux_2_17_x86_64",
        )
        self.rebuild_fixture()

    def write_receipt(self, receipt: dict[str, Any]) -> None:
        self.receipt_path.write_text(json.dumps(receipt), encoding="utf-8")

    def rebuild_fixture(self) -> None:
        build_backend._build_archive(self.stage, self.wheel)
        self.receipt = build_receipt(
            self.wheel,
            name=build_backend.NAME,
            version=build_backend.VERSION,
            tags=[self.tag],
            platform_policy="manylinux_2_17_x86_64",
            kernel_archive_sha256="a" * 64,
            python="3.12.0",
            metadata_version=build_backend.METADATA_VERSION,
            requires_python=build_backend.REQUIRES_PYTHON,
            project_url=build_backend.PROJECT_URL,
            license_files=build_backend._receipt_license_files(),
        )
        self.write_receipt(self.receipt)

    def test_valid_receipt_and_metadata_reach_independent_resource_checks(self) -> None:
        # This fixture deliberately has no kernel. A valid receipt cannot make
        # it pass the product gate, but its metadata and licenses must pass.
        with self.assertRaisesRegex(
            check_python_wheel.WheelCheckError, "wheel static-kernel marker is invalid"
        ):
            check_python_wheel.check(self.args)

    def test_checker_rejects_missing_fields_and_changed_release_identity(self) -> None:
        for field in ("project_url", "tags", "kernel_archive_sha256"):
            with self.subTest(missing=field):
                receipt = dict(self.receipt)
                del receipt[field]
                self.write_receipt(receipt)
                with self.assertRaisesRegex(
                    check_python_wheel.WheelCheckError, "missing required field.*" + field
                ):
                    check_python_wheel.check(self.args)
        for field, value in (
            ("name", "another"), ("version", "0.0.0"),
            ("metadata_version", "2.3"), ("requires_python", ">=3.12"),
            ("project_url", "https://invalid.example"),
            ("license_files", self.receipt["license_files"][:-1]),
        ):
            with self.subTest(changed=field):
                self.write_receipt({**self.receipt, field: value})
                with self.assertRaisesRegex(
                    check_python_wheel.WheelCheckError, field + " differs"
                ):
                    check_python_wheel.check(self.args)

    def test_checker_rechecks_actual_file_identity(self) -> None:
        for field, value, message in (
            ("filename", "missing.whl", "wheel from receipt is missing"),
            ("sha256", "0" * 64, "no longer matches"),
            ("size", self.receipt["size"] + 1, "no longer matches"),
        ):
            with self.subTest(changed=field):
                self.write_receipt({**self.receipt, field: value})
                with self.assertRaisesRegex(check_python_wheel.WheelCheckError, message):
                    check_python_wheel.check(self.args)
        self.write_receipt(self.receipt)
        self.wheel.write_bytes(self.wheel.read_bytes() + b"changed")
        with self.assertRaisesRegex(check_python_wheel.WheelCheckError, "no longer matches"):
            check_python_wheel.check(self.args)

    def test_checker_rechecks_license_contents_even_with_a_matching_receipt(self) -> None:
        (self.dist_info / "licenses/LICENSE").write_bytes(b"changed license")
        self.rebuild_fixture()
        with self.assertRaisesRegex(
            check_python_wheel.WheelCheckError, "license file changed"
        ):
            check_python_wheel.check(self.args)

    def test_checker_rechecks_metadata_even_with_a_matching_receipt(self) -> None:
        metadata = self.dist_info / "METADATA"
        metadata.write_text(
            metadata.read_text(encoding="utf-8").replace(
                build_backend.PROJECT_URL, "https://invalid.example"
            ),
            encoding="utf-8",
        )
        self.rebuild_fixture()
        with self.assertRaisesRegex(
            check_python_wheel.WheelCheckError, "core metadata differs"
        ):
            check_python_wheel.check(self.args)

    def test_checker_rechecks_wheel_tags_and_required_platform(self) -> None:
        self.write_receipt({**self.receipt, "platform_policy": "linux_native"})
        with self.assertRaisesRegex(
            check_python_wheel.WheelCheckError, "does not bind the platform"
        ):
            check_python_wheel.check(self.args)
        (self.dist_info / "WHEEL").write_text(
            "Wheel-Version: 1.0\nTag: cp310-cp310-manylinux_2_17_x86_64\n",
            encoding="utf-8",
        )
        self.rebuild_fixture()
        with self.assertRaisesRegex(
            check_python_wheel.WheelCheckError, "tag does not match its receipt"
        ):
            check_python_wheel.check(self.args)


if __name__ == "__main__":
    unittest.main()
