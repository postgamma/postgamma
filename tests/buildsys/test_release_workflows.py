"""Tests for least-authority public release workflows."""

from __future__ import annotations

import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "buildsys"))

import check_python_release_workflow  # noqa: E402


class ReleaseWorkflowTests(unittest.TestCase):
    def test_repository_workflows_use_exact_tested_bytes(self) -> None:
        report = check_python_release_workflow.check(
            ROOT / ".github/workflows/python-wheels.yml",
            ROOT / "manifests/api/python-api-v1.json",
            ROOT / ".github/workflows/pypi.yml",
        )
        self.assertTrue(report["exact_tested_byte_promotion"])
        self.assertTrue(report["draft_before_approval"])
        self.assertTrue(report["pypi"]["trusted_publishing"])
        self.assertFalse(report["pypi"]["stored_upload_token"])

    def test_pypi_workflow_rejects_a_stored_token(self) -> None:
        content = (ROOT / ".github/workflows/pypi.yml").read_text(encoding="utf-8")
        with tempfile.TemporaryDirectory() as temporary:
            workflow = Path(temporary) / "pypi.yml"
            workflow.write_text(content + "\n# TWINE_PASSWORD\n", encoding="utf-8")
            with self.assertRaisesRegex(
                check_python_release_workflow.WorkflowContractError,
                "stored upload token",
            ):
                check_python_release_workflow.validate_pypi_workflow(workflow)

    def test_manylinux_make_source_must_be_checksum_verified(self) -> None:
        content = (ROOT / ".github/workflows/python-wheels.yml").read_text(
            encoding="utf-8"
        )
        content = content.replace("sha256sum --check", "sha256sum --version")
        with tempfile.TemporaryDirectory() as temporary:
            workflow = Path(temporary) / "python-wheels.yml"
            workflow.write_text(content, encoding="utf-8")
            with self.assertRaisesRegex(
                check_python_release_workflow.WorkflowContractError,
                "GNU Make checksum verification",
            ):
                check_python_release_workflow.check(
                    workflow,
                    ROOT / "manifests/api/python-api-v1.json",
                    ROOT / ".github/workflows/pypi.yml",
                )


if __name__ == "__main__":
    unittest.main()
