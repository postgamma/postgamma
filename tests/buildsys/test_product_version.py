"""Tests for the canonical public version contract."""

from __future__ import annotations

import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "buildsys"))

import product_version  # noqa: E402


class ProductVersionTests(unittest.TestCase):
    def test_supported_public_versions(self) -> None:
        expected = {
            "0.1.0a1": ("alpha", True),
            "0.1.0b2": ("beta", True),
            "0.1.0rc3": ("release-candidate", True),
            "1.0.0": ("stable", False),
        }
        for value, identity in expected.items():
            with self.subTest(value=value):
                parsed = product_version.parse_version(value)
                self.assertEqual((parsed.state, parsed.prerelease), identity)
                self.assertEqual(parsed.tag, f"v{value}")

    def test_release_order_follows_alpha_beta_rc_and_stable(self) -> None:
        ordered = [
            "0.1.0a1",
            "0.1.0a2",
            "0.1.0b1",
            "0.1.0rc1",
            "0.1.0",
            "0.1.1a1",
            "0.1.1",
            "0.2.0a1",
        ]
        keys = [product_version.parse_version(value).ordering_key for value in ordered]
        self.assertEqual(keys, sorted(keys))

    def test_rejects_ambiguous_or_non_release_versions(self) -> None:
        for value in (
            "v0.1.0",
            "0.1",
            "01.2.3",
            "0.1.0-alpha.1",
            "0.1.0a0",
            "0.1.0.dev1",
            "0.1.0.post1",
            "0.1.0+linux",
        ):
            with self.subTest(value=value):
                with self.assertRaises(product_version.ProductVersionError):
                    product_version.parse_version(value)

    def test_repository_release_identity_is_closed(self) -> None:
        report = product_version.repository_identity(ROOT, "v0.1.0a1")
        self.assertEqual(report["version"], "0.1.0a1")
        self.assertEqual(report["state"], "alpha")
        self.assertEqual(report["c_build_id"], "postgamma-0.1.0a1-pg19")

    def test_rejects_a_mismatched_tag(self) -> None:
        with self.assertRaisesRegex(
            product_version.ProductVersionError, "does not match canonical tag"
        ):
            product_version.repository_identity(ROOT, "v0.1.0a2")


if __name__ == "__main__":
    unittest.main()
