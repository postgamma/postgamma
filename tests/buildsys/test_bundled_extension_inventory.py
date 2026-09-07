from __future__ import annotations

import json
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "buildsys"))

from check_bundled_extension_inventory import (  # noqa: E402
    BundledExtensionInventoryError,
    validate_policy,
    validate_virtualized_policy,
)


class BundledExtensionInventoryTests(unittest.TestCase):
    def test_accepts_zero_mutable_static_state(self) -> None:
        result = validate_policy(
            ROOT / "manifests/ownership/extensions/postgamma-sdk-probe.json",
            "postgamma_sdk_probe",
            set(),
        )
        self.assertEqual(result["mutable_static_candidates"], 0)

    def test_rejects_an_unreviewed_mutable_static(self) -> None:
        with self.assertRaisesRegex(
            BundledExtensionInventoryError, "mutable static storage"
        ):
            validate_policy(
                ROOT / "manifests/ownership/extensions/postgamma-sdk-probe.json",
                "postgamma_sdk_probe",
                {"file_scope:probe.c:counter"},
            )

    def test_validates_virtualized_kernel_reference_closure(self) -> None:
        policy = {
            "schema_version": 1,
            "kind": "postgamma.extension-state-ownership",
            "extension_id": "fixture",
            "accessor": "fixture_state_address",
            "upstream": {
                "repository": "https://example.invalid/fixture.git",
                "commit": "0123456789abcdef",
                "version": "1.0",
            },
            "decisions": [
                {
                    "id": "external:fixture_state",
                    "owner": "role",
                    "rationale": "The state belongs to one role",
                }
            ],
        }
        report = {
            "schema_version": 1,
            "kind": "postgamma.extension-state-alignment",
            "status": "pass",
            "extension_id": "fixture",
            "upstream": policy["upstream"],
            "candidates": [{"id": "external:fixture_state"}],
            "kernel_references": ["external:kernel_state"],
            "summary": {
                "candidate_count": 1,
                "role_candidate_count": 1,
                "replacement_count": 2,
                "static_initializer_use_count": 0,
                "kernel_reference_count": 1,
                "kernel_reference_symbol_count": 1,
                "kernel_state_replacement_count": 1,
                "kernel_guc_replacement_count": 0,
                "kernel_immutable_reference_count": 0,
            },
            "selection": {"domain": "fixture", "translation_units": 1},
        }
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            policy_path = root / "policy.json"
            report_path = root / "report.json"
            policy_path.write_text(json.dumps(policy), encoding="utf-8")
            report_path.write_text(json.dumps(report), encoding="utf-8")
            result = validate_virtualized_policy(
                policy_path, "fixture", report_path
            )
            self.assertEqual(result["kernel_reference_count"], 1)
            report["summary"]["kernel_reference_count"] = 2
            report_path.write_text(json.dumps(report), encoding="utf-8")
            with self.assertRaisesRegex(
                BundledExtensionInventoryError,
                "virtualized state evidence is incomplete",
            ):
                validate_virtualized_policy(policy_path, "fixture", report_path)

if __name__ == "__main__":
    unittest.main()
