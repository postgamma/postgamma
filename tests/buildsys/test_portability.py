"""Tests for source portability checks."""

from __future__ import annotations

import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "buildsys"))

import check_portability  # noqa: E402


class PortabilityTests(unittest.TestCase):
    def test_third_party_trees_are_not_project_owned(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "owned").mkdir()
            (root / "owned" / "source.c").write_text("owned", encoding="utf-8")
            (root / "third_party" / "vendor").mkdir(parents=True)
            (root / "third_party" / "vendor" / "source.c").write_text(
                "vendored", encoding="utf-8"
            )
            relative = {
                path.relative_to(root) for path in check_portability.project_files(root)
            }
            self.assertEqual(relative, {Path("owned/source.c")})


if __name__ == "__main__":
    unittest.main()
