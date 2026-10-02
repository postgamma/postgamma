"""Tests for the user-facing terminology boundary."""

from __future__ import annotations

import sys
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "buildsys"))

import check_public_surface  # noqa: E402


class PublicSurfaceTests(unittest.TestCase):
    def test_recognizes_only_the_incorrect_brand_as_a_complete_token(self) -> None:
        self.assertEqual(
            check_public_surface.incorrect_brand_lines(
                "postgamma\nPostGamma\nPostgamma\nPostgammaInstanceRuntime\n"
            ),
            [3],
        )

    def test_distinguishes_prose_brand_from_wordmarks_and_technical_names(self) -> None:
        self.assertEqual(
            check_public_surface.lowercase_prose_brand_lines(
                "PostGamma is embedded.\n"
                "postgamma is embedded.\n"
                "`postgamma` package\n"
                "postgamma.com and libpostgamma.a\n"
                "```python\nimport postgamma\n```\n"
                '<a aria-label="postgamma home">\n'
                '<span class="pg-brand-wordmark">postgamma</span></a>\n'
                "site_name: postgamma\n"
                '<p class="description">postgamma is embedded.</p>\n'
            ),
            [2, 11],
        )

    def test_repository_public_surface_uses_product_terms(self) -> None:
        report = check_public_surface.check(ROOT, "make")
        self.assertEqual(report["status"], "pass")
        self.assertEqual(report["incorrect_brand_violations"], 0)
        self.assertEqual(report["lowercase_prose_brand_violations"], 0)
        self.assertEqual(report["display_brand"], "PostGamma")
        self.assertEqual(report["build_id"], "postgamma-0.1.0a1-pg19")
        self.assertEqual(report["copyright_owner"], "Shujie Zhang")
        self.assertGreaterEqual(report["copyrighted_product_source_count"], 90)
        self.assertEqual(report["official_site"], "https://postgamma.com")


if __name__ == "__main__":
    unittest.main()
