"""Tests for the SDK release ABI and evidence closure."""

from __future__ import annotations

import sys
import tempfile
import unittest
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(PROJECT_ROOT / "buildsys"))

import check_c_api_release  # noqa: E402


class ReleaseClosureTests(unittest.TestCase):
    def setUp(self) -> None:
        self.release = check_c_api_release.load_document(
            PROJECT_ROOT / "manifests/api/c-abi-v1-1.json",
            check_c_api_release.RELEASE_KIND,
        )

    def test_release_is_exactly_six_additive_functions(self) -> None:
        names = {
            entry["name"] for entry in self.release["additive_functions"]
        }
        self.assertEqual(
            names,
            {
                "pgm_instance_checkpoint_async",
                "pgm_operation_cancel",
                "pgm_operation_free",
                "pgm_operation_progress",
                "pgm_operation_waitable",
                "pgm_result_export_arrow",
            },
        )

    def test_release_requires_complete_evidence_graph(self) -> None:
        names = {
            entry["name"] for entry in self.release["required_evidence"]
        }
        self.assertEqual(len(names), 20)
        self.assertIn("installed_sdk", names)
        self.assertIn("sanitizers", names)
        self.assertIn("portability", names)

    def test_duplicate_named_evidence_is_rejected(self) -> None:
        with self.assertRaisesRegex(
            check_c_api_release.ReleaseCheckError, "invalid named evidence"
        ):
            check_c_api_release.parse_named_paths(["one=/a", "one=/b"])

    def test_non_object_layout_field_is_rejected(self) -> None:
        with self.assertRaisesRegex(
            check_c_api_release.ReleaseCheckError, "non-object field"
        ):
            check_c_api_release.normalize_fields(
                {"name": "broken", "fields": ["not-an-object"]}
            )

    def test_newer_minor_version_may_add_exported_symbols(self) -> None:
        count = check_c_api_release.validate_exported_symbols(
            {"pgm_released"},
            ["pgm_released", "pgm_new_minor_version"],
            1,
        )
        self.assertEqual(count, 2)

    def test_newer_minor_version_may_not_remove_released_symbols(self) -> None:
        with self.assertRaisesRegex(
            check_c_api_release.ReleaseCheckError,
            "symbol allowlist changed",
        ):
            check_c_api_release.validate_exported_symbols(
                {"pgm_released"},
                ["pgm_new_minor_version"],
                1,
            )

    def test_documentation_tracks_promoted_capabilities(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            readme = Path(temporary_directory) / "README.md"
            readme.write_text(
                "\n".join(self.release["documented_unsupported"]),
                encoding="utf-8",
            )
            documented = check_c_api_release.validate_documentation(
                self.release,
                readme,
                {
                    "numeric_registry": {
                        "values": {"PGM_CAP_MULTIPLE_INSTANCES": 1024}
                    }
                },
                {"marker": {"capabilities": "1024"}},
            )

        self.assertEqual(documented["promoted"], ["multiple live instances"])
        self.assertNotIn(
            "multiple live instances", documented["unsupported"]
        )

    def test_documentation_retains_unadvertised_capabilities(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            readme = Path(temporary_directory) / "README.md"
            readme.write_text(
                "\n".join(self.release["documented_unsupported"]),
                encoding="utf-8",
            )
            documented = check_c_api_release.validate_documentation(
                self.release,
                readme,
                {
                    "numeric_registry": {
                        "values": {"PGM_CAP_MULTIPLE_INSTANCES": 1024}
                    }
                },
                {"marker": {"capabilities": "0"}},
            )

        self.assertEqual(documented["promoted"], [])
        self.assertIn("multiple live instances", documented["unsupported"])


if __name__ == "__main__":
    unittest.main()
