"""Tests for the C API sanitizer evidence collector."""

from __future__ import annotations

import sys
import tempfile
import unittest
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(PROJECT_ROOT / "buildsys"))

import check_release_sanitizers  # noqa: E402


class SanitizerEvidenceTests(unittest.TestCase):
    def test_collects_both_families(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            for stem in ("first", "second"):
                for suffix in ("asan", "tsan"):
                    (directory / f"{stem}-{suffix}").write_bytes(b"program")
            programs = check_release_sanitizers.collect_programs(
                directory, ("first", "second")
            )
            self.assertEqual(len(programs["asan_ubsan_lsan"]), 2)
            self.assertEqual(len(programs["tsan"]), 2)

    def test_missing_program_fails_closed(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            with self.assertRaisesRegex(
                check_release_sanitizers.SanitizerCheckError,
                "missing tsan program",
            ):
                directory = Path(temporary)
                (directory / "only-asan").write_bytes(b"program")
                check_release_sanitizers.collect_programs(directory, ("only",))


if __name__ == "__main__":
    unittest.main()
