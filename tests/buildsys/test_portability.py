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
    def test_vendor_and_git_metadata_are_not_project_owned(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "owned").mkdir()
            (root / "owned" / "source.c").write_text("owned", encoding="utf-8")
            (root / "third_party" / "vendor").mkdir(parents=True)
            (root / "third_party" / "vendor" / "source.c").write_text(
                "vendored", encoding="utf-8"
            )
            nested = root / ".claude/worktrees/preview"
            nested.mkdir(parents=True)
            (nested / "source.c").write_text("nested source", encoding="utf-8")
            git_paths = (root / ".git", nested / ".git")
            for git_path in git_paths:
                git_path.write_text("gitdir: /work/main/.git/worktrees/preview\n")
            expected = {
                Path("owned/source.c"),
                Path(".claude/worktrees/preview/source.c"),
            }
            relative = {
                path.relative_to(root) for path in check_portability.project_files(root)
            }
            self.assertEqual(relative, expected)
            for git_path in git_paths:
                git_path.unlink()
                git_path.mkdir()
                (git_path / "HEAD").write_text("ref: refs/heads/main\n")
            self.assertEqual(
                check_portability.project_files(root),
                sorted(root / path for path in expected),
            )


if __name__ == "__main__":
    unittest.main()
