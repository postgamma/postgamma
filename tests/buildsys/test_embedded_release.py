"""Tests for the embedded release gate."""

from __future__ import annotations

import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "buildsys"))

from check_c_api_release import load_document  # noqa: E402
from check_embedded_release import (  # noqa: E402
    BASELINE_KIND,
    EmbeddedReleaseCheckError,
    exact_abi_metadata,
    validate_canary_contract,
)


class EmbeddedReleaseTests(unittest.TestCase):
    def test_accepts_frozen_abi_metadata(self) -> None:
        baseline = load_document(
            ROOT / "manifests/api/c-abi-v1-3.json", BASELINE_KIND
        )
        self.assertEqual(exact_abi_metadata(baseline)["encoded_version"], 65539)

    def test_accepts_weekly_moving_canary_with_manual_profiles(self) -> None:
        result = validate_canary_contract(
            ROOT / ".github/workflows/upstream-canary.yml"
        )
        self.assertEqual(result["weekly_product_gate"], "complete-source-check")
        self.assertEqual(result["moving_refs"], ["REL_19_STABLE", "master"])

    def test_rejects_canary_without_master(self) -> None:
        source = (ROOT / ".github/workflows/upstream-canary.yml").read_text(
            encoding="utf-8"
        )
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "canary.yml"
            path.write_text(source.replace("          - master\n", ""), encoding="utf-8")
            with self.assertRaisesRegex(EmbeddedReleaseCheckError, "incomplete"):
                validate_canary_contract(path)


if __name__ == "__main__":
    unittest.main()
