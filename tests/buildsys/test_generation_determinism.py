"""Tests for generation-determinism workspace containment."""

from __future__ import annotations

import json
import sys
import tempfile
import unittest
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(PROJECT_ROOT / "tests/buildsys"))

import verify_generation_determinism as determinism  # noqa: E402


class GenerationDeterminismTests(unittest.TestCase):
    def test_external_build_directory_accepts_its_determinism_workspace(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            base = Path(temporary) / "external-build"
            workspace = base / "tests/determinism"
            determinism.require_build_child(workspace, base)

    def test_workspace_must_be_a_real_child_of_selected_build_directory(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            base = root / "selected-build"
            outside = root / "other-build/tests/determinism"
            with self.assertRaisesRegex(ValueError, "must be below"):
                determinism.require_build_child(outside, base)
            with self.assertRaisesRegex(ValueError, "complete build directory"):
                determinism.require_build_child(base, base)

    def test_symlink_cannot_escape_selected_build_directory(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            base = root / "selected-build"
            outside = root / "outside"
            base.mkdir()
            outside.mkdir()
            (base / "escape").symlink_to(outside, target_is_directory=True)
            with self.assertRaisesRegex(ValueError, "must be below"):
                determinism.require_build_child(base / "escape/work", base)

    def test_artifact_install_uses_selected_external_build_directory(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            base = Path(temporary) / "external-build"
            template = base / "reference"
            destination = base / "tests/determinism/artifacts"
            plan = base / "tests/determinism/generated-plan.json"
            template.mkdir(parents=True)
            plan.parent.mkdir(parents=True)
            (template / ".postgamma-configure.json").write_text(
                "{}\n", encoding="ascii"
            )
            plan.write_text(
                json.dumps({"schema_version": 1, "mode": "plan", "files": []}),
                encoding="ascii",
            )

            determinism.install_generated_artifacts(
                PROJECT_ROOT, plan, template, destination, base
            )

            self.assertTrue(
                (destination / ".postgamma-generated-replacements.json").is_file()
            )


if __name__ == "__main__":
    unittest.main()
