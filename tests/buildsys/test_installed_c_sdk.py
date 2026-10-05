"""Tests for the standalone C API installed-SDK gate."""

from __future__ import annotations

import sys
import tempfile
import unittest
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(PROJECT_ROOT / "buildsys"))

import check_installed_c_sdk  # noqa: E402


class InstalledSdkTests(unittest.TestCase):
    def test_accepts_complete_quickstart_marker(self) -> None:
        line = check_installed_c_sdk.MARKER + " " + " ".join(
            f"{name}={value}"
            for name, value in check_installed_c_sdk.MARKER_VALUES.items()
        )
        values = check_installed_c_sdk.marker_fields(line + "\n")
        self.assertEqual(values["checkpoint"], "true")

    def test_rejects_capability_drift(self) -> None:
        values = {
            **check_installed_c_sdk.MARKER_VALUES,
            "capabilities": "511",
        }
        line = check_installed_c_sdk.MARKER + " " + " ".join(
            f"{name}={value}" for name, value in values.items()
        )
        with self.assertRaisesRegex(
            check_installed_c_sdk.InstalledSdkCheckError,
            "capabilities",
        ):
            check_installed_c_sdk.marker_fields(line + "\n")

    def test_staged_paths_are_relative_to_sdk_root(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "source"
            source.write_bytes(b"library")
            sdk = root / "sdk"
            record = check_installed_c_sdk.stage_file(
                source, sdk / "lib" / "libpostgamma.so", sdk
            )
            self.assertEqual(record["path"], "lib/libpostgamma.so")


if __name__ == "__main__":
    unittest.main()
