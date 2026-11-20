"""Build artifact publication must preserve evidence on failure and make timestamps."""

import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "buildsys"))

import artifact_io  # noqa: E402
import check_bootstrap_evidence  # noqa: E402


class ArtifactIoTests(unittest.TestCase):
    def test_failed_publication_preserves_previous_evidence(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "evidence.json"
            path.write_text("previous evidence\n", encoding="utf-8")
            with patch.object(artifact_io.os, "replace", side_effect=OSError("full")):
                with self.assertRaises(OSError):
                    artifact_io.write_json(path, {"result": "new"})
            self.assertEqual(path.read_text(), "previous evidence\n")
            self.assertEqual(list(path.parent.iterdir()), [path])

    def test_generated_output_advances_timestamp_without_replacing_unchanged_inode(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "nested" / "generated.c"
            artifact_io.write_generated_text(path, "generated\n")
            os.utime(path, (1, 1))
            before = path.stat()
            artifact_io.write_generated_text(path, "generated\n")
            after = path.stat()
            self.assertEqual(after.st_ino, before.st_ino)
            self.assertGreater(after.st_mtime_ns, before.st_mtime_ns)

    def test_json_evidence_keeps_distinct_rewrite_and_write_if_changed_policies(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            for writer, rewrite in (
                (artifact_io.write_json, True),
                (check_bootstrap_evidence.write_json, False),
            ):
                with self.subTest(writer=writer.__module__):
                    path = Path(temporary) / writer.__module__ / "result.json"
                    writer(path, {"b": 2, "a": 1})
                    self.assertEqual(path.read_text(), '{\n  "a": 1,\n  "b": 2\n}\n')
                    os.utime(path, (1, 1))
                    before = path.stat()
                    writer(path, {"a": 1, "b": 2})
                    after = path.stat()
                    self.assertEqual(after.st_mtime_ns > before.st_mtime_ns, rewrite)
                    self.assertEqual(after.st_ino != before.st_ino, rewrite)
