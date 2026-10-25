"""Artifact handoff checks preserve bytes and reject incomplete SDK payloads."""

from __future__ import annotations

import argparse
import hashlib
import io
import json
import subprocess
import sys
import tarfile
import tempfile
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "buildsys"))

from check_full_test_artifacts import check, export_artifacts, safe_path, unpack_sdk
from check_installed_c_sdk import MARKER, MARKER_VALUES
from check_python_wheel import require_platform_policy
from check_runtime_libraries import RuntimeLibraryError
from check_static_sdk import PGVECTOR_MARKER, PGVECTOR_MARKER_VALUES


class FullTestArtifactTests(unittest.TestCase):
    def test_wheel_handoff_selects_native_checks_and_preserves_manylinux_policy(self):
        commit = subprocess.check_output(
            ["git", "-C", str(ROOT), "rev-parse", "HEAD"], text=True,
        ).strip()
        sdk_output = "\n".join(
            marker + " " + " ".join(f"{key}={value}" for key, value in values.items())
            for marker, values in (
                (MARKER, MARKER_VALUES), (PGVECTOR_MARKER, PGVECTOR_MARKER_VALUES),
            )
        )
        sdk_result = subprocess.CompletedProcess([], 0, stdout=sdk_output, stderr="")
        for policy, tag, required, outcome in (
            ("linux_native", "cp311-cp311-linux_x86_64", None, "wheel check reached"),
            ("manylinux_2_17_x86_64", "cp311-cp311-manylinux2014_x86_64",
             "manylinux_2_17_x86_64", "wheel check reached"),
            ("manylinux_2_17_x86_64", "cp311-cp311-linux_x86_64",
             "manylinux_2_17_x86_64", "wheel tags exceed required policy"),
            ("unknown", "cp311-cp311-linux_x86_64", "unknown", "unsupported wheel platform policy"),
        ):
            with self.subTest(policy=policy, tag=tag), tempfile.TemporaryDirectory() as temporary:
                directory = Path(temporary)
                artifacts = directory / "artifacts"
                artifacts.mkdir()
                files = {
                    "sdk.tar.gz": b"SDK fixture",
                    "receipt.json": json.dumps({
                        "filename": "postgamma-fixture.whl", "platform_policy": policy,
                    }).encode(),
                    "postgamma-fixture.whl": b"wheel fixture",
                }
                for name, content in files.items():
                    (artifacts / name).write_bytes(content)
                (artifacts / "manifest.json").write_text(json.dumps({
                    "schema_version": 1, "kind": "postgamma.full-test-artifacts",
                    "source_commit": commit, "sdk": "sdk.tar.gz", "wheel_receipt": "receipt.json",
                    "files": {name: hashlib.sha256(content).hexdigest() for name, content in files.items()},
                }))
                args = argparse.Namespace(
                    root=ROOT, artifacts=artifacts, work_root=directory / "work",
                    cc="cc", pkg_config="pkg-config", readelf="readelf", python=sys.executable,
                    forbidden_prefix=[], maximum_glibc=None,
                )

                def wheel_check(wheel_args):
                    self.assertEqual(wheel_args.require_platform, required)
                    self.assertEqual(wheel_args.installer, "pip")
                    self.assertEqual(wheel_args.receipt, artifacts / "receipt.json")
                    if wheel_args.require_platform is not None:
                        require_platform_policy([tag], wheel_args.require_platform)
                    raise RuntimeError("wheel check reached")

                sdk = directory / "sdk"
                # Stub SDK execution so this test reaches the wheel handoff
                # without building a kernel. The platform gate itself is real.
                with (
                    mock.patch("check_full_test_artifacts.unpack_sdk", return_value=sdk),
                    mock.patch("check_full_test_artifacts.subprocess.check_output", side_effect=[
                        commit, str(sdk / "lib/libpostgamma.a"),
                    ]),
                    mock.patch("check_full_test_artifacts.subprocess.run", return_value=sdk_result),
                    mock.patch("check_full_test_artifacts.run_quickstart", return_value=sdk_result),
                    mock.patch("check_full_test_artifacts.inspect_libraries", return_value={}),
                    mock.patch("check_full_test_artifacts.check_wheel", side_effect=wheel_check) as checker,
                    self.assertRaisesRegex(RuntimeError, outcome),
                ):
                    check(args)
                checker.assert_called_once()

    def test_handoff_preserves_exact_artifacts_and_rejects_subsequent_changes(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            build = directory / "build"
            (build / "dist").mkdir(parents=True)
            sdk = build / "dist/postgamma-sdk-fixture.tar.gz"
            sdk.write_bytes(b"SDK archive fixture")
            (build / "python-product/reports").mkdir(parents=True)
            wheel_dir = build / "python-product/wheel"
            wheel_dir.mkdir(parents=True)
            wheel = wheel_dir / "postgamma-fixture.whl"
            wheel.write_bytes(b"wheel fixture")
            receipt = build / "python-product/reports/wheel-build.json"
            receipt.write_text(json.dumps({"filename": wheel.name, "sha256": hashlib.sha256(wheel.read_bytes()).hexdigest()}))
            output = directory / "artifacts"
            export_artifacts(ROOT, build, output)
            self.assertEqual((output / "sdk" / sdk.name).read_bytes(), sdk.read_bytes())
            self.assertEqual((output / "wheel" / wheel.name).read_bytes(), wheel.read_bytes())
            manifest = json.loads((output / "manifest.json").read_text())
            self.assertEqual(manifest["source_commit"], subprocess.check_output(
                ["git", "-C", str(ROOT), "rev-parse", "HEAD"], text=True,
            ).strip())
            (output / "wheel" / wheel.name).write_bytes(b"changed wheel")
            with self.assertRaisesRegex(RuntimeLibraryError, "checksum mismatch"):
                check(argparse.Namespace(root=ROOT, artifacts=output))
            (output / "wheel/stale.whl").write_bytes(b"old artifact")
            export_artifacts(ROOT, build, output)
            self.assertFalse((output / "wheel/stale.whl").exists())
            manifest["source_commit"] = "0" * 40
            (output / "manifest.json").write_text(json.dumps(manifest))
            with self.assertRaisesRegex(RuntimeLibraryError, "source commit"):
                check(argparse.Namespace(root=ROOT, artifacts=output))

    def test_sdk_extraction_verifies_every_file_and_rejects_links(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            data = b"SDK content"
            manifest = json.dumps({
                "schema_version": 1, "kind": "postgamma.static-sdk-manifest",
                "files": [{"path": "lib/libpostgamma.a", "sha256": hashlib.sha256(data).hexdigest()}],
            }).encode()
            for scenario in ("valid", "changed", "extra", "symlink", "duplicate"):
                with self.subTest(scenario=scenario):
                    archive_path = directory / (scenario + ".tar")
                    with tarfile.open(archive_path, "w") as archive:
                        entries = [
                            ("sdk/MANIFEST.json", manifest),
                            ("sdk/lib/libpostgamma.a", b"changed" if scenario == "changed" else data),
                        ]
                        if scenario == "extra":
                            entries.append(("sdk/unlisted", b"extra"))
                        if scenario == "duplicate":
                            entries.append(entries[-1])
                        for name, content in entries:
                            member = tarfile.TarInfo(name)
                            member.size = len(content)
                            archive.addfile(member, io.BytesIO(content))
                        if scenario == "symlink":
                            member = tarfile.TarInfo("sdk/link")
                            member.type = tarfile.SYMTYPE
                            member.linkname = "../../outside"
                            archive.addfile(member)
                    destination = directory / scenario
                    if scenario == "valid":
                        sdk = unpack_sdk(archive_path, destination)
                        self.assertEqual((sdk / "lib/libpostgamma.a").read_bytes(), data)
                    else:
                        with self.assertRaises(RuntimeLibraryError):
                            unpack_sdk(archive_path, destination)

    def test_artifact_names_cannot_escape_the_handoff_directory(self):
        for name in ("", ".", "..", "../outside", "/absolute", "a/../outside", "a//b"):
            with self.subTest(name=name), self.assertRaises(RuntimeLibraryError):
                safe_path(name)


if __name__ == "__main__":
    unittest.main()
