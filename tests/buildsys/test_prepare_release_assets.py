"""Tests for promotion of exact tested release bytes."""

from __future__ import annotations

import hashlib
import json
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "buildsys"))

import prepare_release_assets  # noqa: E402


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


class ReleaseAssetTests(unittest.TestCase):
    def fixture(self, temporary: Path) -> dict[str, Path]:
        artifacts = temporary / "python"
        entries = []
        for tag in ("cp310", "cp311"):
            wheel = (
                artifacts
                / tag
                / "wheel"
                / (f"postgamma-0.1.0a1-{tag}-{tag}-manylinux_2_28_x86_64.whl")
            )
            wheel.parent.mkdir(parents=True)
            wheel.write_bytes(f"tested {tag}\n".encode())
            entries.append(
                {
                    "python_tag": tag,
                    "filename": wheel.name,
                    "sha256": digest(wheel),
                }
            )
        evidence = temporary / "python-evidence.json"
        evidence.write_text(
            json.dumps(
                {
                    "schema_version": 1,
                    "kind": "postgamma.python-release-evidence",
                    "status": "pass",
                    "release": {"version": "0.1.0a1"},
                    "postgresql": {"major": 19},
                    "wheel_count": len(entries),
                    "wheels": entries,
                    "distribution_matrix_verified": True,
                    "public_release_ready": True,
                    "publish_authorized": False,
                }
            ),
            encoding="utf-8",
        )
        archive = temporary / "postgamma-sdk-0.1.0a1-linux-x86_64.tar.gz"
        archive.write_bytes(b"tested static sdk\n")
        checksum = archive.with_suffix(archive.suffix + ".sha256")
        checksum.write_text(f"{digest(archive)}  {archive.name}\n", encoding="ascii")
        receipt = temporary / "static.json"
        receipt.write_text(
            json.dumps(
                {
                    "schema_version": 1,
                    "kind": "postgamma.static-sdk-archive",
                    "status": "pass",
                    "product_version": "0.1.0a1",
                    "archive": archive.name,
                    "checksum": checksum.name,
                    "archive_sha256": digest(archive),
                }
            ),
            encoding="utf-8",
        )
        return {
            "artifacts": artifacts,
            "evidence": evidence,
            "archive": archive,
            "checksum": checksum,
            "receipt": receipt,
        }

    def test_prepares_and_reverifies_exact_wheels(self) -> None:
        with tempfile.TemporaryDirectory() as name:
            temporary = Path(name)
            fixture = self.fixture(temporary)
            output = temporary / "release"
            manifest = prepare_release_assets.prepare(
                root=ROOT,
                python_artifacts=fixture["artifacts"],
                python_evidence_path=fixture["evidence"],
                static_archive=fixture["archive"],
                static_checksum=fixture["checksum"],
                static_receipt_path=fixture["receipt"],
                output=output,
            )
            self.assertTrue(manifest["promotion"]["exact_tested_bytes"])
            report = prepare_release_assets.verify_wheels(ROOT, output, "v0.1.0a1")
            self.assertEqual(report["wheel_count"], 2)

    def test_rejects_a_wheel_changed_after_testing(self) -> None:
        with tempfile.TemporaryDirectory() as name:
            temporary = Path(name)
            fixture = self.fixture(temporary)
            next(fixture["artifacts"].rglob("*.whl")).write_bytes(b"changed\n")
            with self.assertRaisesRegex(
                prepare_release_assets.ReleaseAssetError, "tested digest differs"
            ):
                prepare_release_assets.prepare(
                    root=ROOT,
                    python_artifacts=fixture["artifacts"],
                    python_evidence_path=fixture["evidence"],
                    static_archive=fixture["archive"],
                    static_checksum=fixture["checksum"],
                    static_receipt_path=fixture["receipt"],
                    output=temporary / "release",
                )

    def test_rejects_a_nonempty_output_directory(self) -> None:
        with tempfile.TemporaryDirectory() as name:
            temporary = Path(name)
            fixture = self.fixture(temporary)
            output = temporary / "release"
            output.mkdir()
            (output / "stale").write_text("stale\n", encoding="utf-8")
            with self.assertRaisesRegex(
                prepare_release_assets.ReleaseAssetError, "empty directory"
            ):
                prepare_release_assets.prepare(
                    root=ROOT,
                    python_artifacts=fixture["artifacts"],
                    python_evidence_path=fixture["evidence"],
                    static_archive=fixture["archive"],
                    static_checksum=fixture["checksum"],
                    static_receipt_path=fixture["receipt"],
                    output=output,
                )


if __name__ == "__main__":
    unittest.main()
