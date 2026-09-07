"""Tests for the public static SDK consumer gate."""

from __future__ import annotations

import sys
import tempfile
import unittest
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(PROJECT_ROOT / "buildsys"))

import check_static_sdk  # noqa: E402


class StaticSdkCheckTests(unittest.TestCase):
    def test_accepts_relocatable_pkg_config_archive_path(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            sdk = Path(temporary) / "sdk"
            archive = sdk / "lib/libpostgamma.a"
            flag = sdk / "lib/pkgconfig/../../lib/libpostgamma.a"
            self.assertTrue(
                check_static_sdk.selects_static_archive([str(flag)], archive)
            )

    def test_rejects_a_different_archive(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            sdk = Path(temporary) / "sdk"
            archive = sdk / "lib/libpostgamma.a"
            other = sdk / "lib/libother.a"
            self.assertFalse(
                check_static_sdk.selects_static_archive([str(other)], archive)
            )

    def test_accepts_exact_static_pgvector_marker(self) -> None:
        marker = check_static_sdk.pgvector_marker_fields(
            "POSTGAMMA_STATIC_PGVECTOR version=0.8.6 hnsw=true "
            "nearest=1 phase=closed\n"
        )
        self.assertEqual(marker, check_static_sdk.PGVECTOR_MARKER_VALUES)

    def test_rejects_incomplete_static_pgvector_marker(self) -> None:
        with self.assertRaisesRegex(
            check_static_sdk.StaticSdkCheckError, "marker changed"
        ):
            check_static_sdk.pgvector_marker_fields(
                "POSTGAMMA_STATIC_PGVECTOR version=0.8.6 hnsw=true "
                "phase=closed\n"
            )


if __name__ == "__main__":
    unittest.main()
