"""Tests for the embedded release gate."""

from __future__ import annotations

import copy
import json
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
    validate_abi,
)


class EmbeddedReleaseTests(unittest.TestCase):
    def test_accepts_frozen_abi_metadata(self) -> None:
        baseline = load_document(ROOT / "manifests/api/c-abi-v1-4.json", BASELINE_KIND)
        self.assertEqual(exact_abi_metadata(baseline)["encoded_version"], 65540)

    def test_new_abi_still_rejects_changes_to_frozen_13_surface(self) -> None:
        def read(name):
            return json.loads((ROOT / "manifests/api" / name).read_text())

        core = read("c-core-abi-v1.json")
        baseline = read("c-abi-v1-4.json")
        functions = copy.deepcopy(core["functions"])
        records = [
            dict(r, complete=True, size=r["minimum_size"]) for r in core["records"]
        ]
        records += [dict(name=name, complete=False) for name in core["opaque_records"]]
        for version in (1, 2, 3, 4):
            released = read(f"c-abi-v1-{version}.json")
            functions += released["additive_functions"]
            records += [
                dict(r, complete=True) for r in released["additive_complete_records"]
            ]
            records += [
                dict(name=name, complete=False)
                for name in released.get("additive_opaque_records", [])
            ]
        catalog = {"functions": functions, "records": records}
        policy = read("c-public-api.json")
        numerics = {
            name: value
            for group in policy["numeric_registry"]
            for name, value in group["values"].items()
        }
        closure = {
            "status": "pass",
            "abi": baseline["abi"],
            "core_abi_baseline": {"breaking_changes": 0},
            "numeric_registry": {"values": numerics},
        }
        prior = {
            "status": "pass",
            "abi": {
                "encoded_version": 65538,
                "prior_abi_compatible": True,
                "breaking_changes": 0,
                "functions": {"prior": 69, "additive": 4},
                "records": {"prior_complete": 17, "additive_complete": 4},
            },
        }
        api = {
            "status": "pass",
            "marker": {"abi": "65540", "postgres": "19", "capabilities": "112639"},
            "dynamic_symbols": {
                "version_node": "POSTGAMMA_1.0",
                "exported_symbols": [f["name"] for f in functions],
                "exported_symbol_count": 76,
            },
        }
        self.assertEqual(
            validate_abi(baseline, prior, catalog, closure, api)["function_count"], 76
        )
        for field in ("signature", "layout", "numeric"):
            with self.subTest(field=field):
                changed, changed_closure = copy.deepcopy(catalog), copy.deepcopy(
                    closure
                )
                if field == "signature":
                    next(
                        f
                        for f in changed["functions"]
                        if f["name"] == "pgm_bundled_extension_count"
                    )["function_type"] = "void (void)"
                elif field == "layout":
                    next(
                        r
                        for r in changed["records"]
                        if r["name"] == "pgm_bundled_extension_info"
                    )["size"] = 56
                else:
                    changed_closure["numeric_registry"]["values"][
                        "PGM_CAP_BUNDLED_EXTENSIONS"
                    ] = 0
                with self.assertRaises(EmbeddedReleaseCheckError):
                    validate_abi(baseline, prior, changed, changed_closure, api)

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
            path.write_text(
                source.replace("          - master\n", ""), encoding="utf-8"
            )
            with self.assertRaisesRegex(EmbeddedReleaseCheckError, "incomplete"):
                validate_canary_contract(path)


if __name__ == "__main__":
    unittest.main()
