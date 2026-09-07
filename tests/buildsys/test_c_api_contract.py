"""Tests for the executable C API public API contract gate."""

from __future__ import annotations

import copy
import json
import sys
import unittest
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(PROJECT_ROOT / "buildsys"))

import check_c_api_contract  # noqa: E402


class CApiContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.policy = json.loads(
            (PROJECT_ROOT / "manifests/api/c-public-api.json").read_text(
                encoding="utf-8"
            )
        )
        cls.protocol = json.loads(
            (PROJECT_ROOT / "manifests/api/postgresql-19-protocol.json").read_text(
                encoding="utf-8"
            )
        )
        cls.baseline = json.loads(
            (PROJECT_ROOT / "manifests/api/c-core-abi-v1.json").read_text(
                encoding="utf-8"
            )
        )

    def test_parses_numeric_layout_and_field_facts(self) -> None:
        facts = check_c_api_contract.parse_consumer_output(
            "numeric PGM_STATUS_OK 0\n"
            "layout pgm_sample 16 8\n"
            "field pgm_sample value 8\n"
        )
        self.assertEqual(facts["numerics"], {"PGM_STATUS_OK": 0})
        self.assertEqual(facts["layouts"]["pgm_sample"]["fields"], {"value": 8})

    def test_rejects_duplicate_numeric_facts(self) -> None:
        with self.assertRaisesRegex(
            check_c_api_contract.ApiContractError, "duplicate numeric"
        ):
            check_c_api_contract.parse_consumer_output(
                "numeric PGM_ONE 1\nnumeric PGM_ONE 1\nlayout pgm_x 1 1\n"
            )

    def test_numeric_registry_matches_reviewed_values(self) -> None:
        numerics = check_c_api_contract.flatten_numeric_registry(self.policy)
        self.assertEqual(numerics["PGM_ABI_VERSION"], 65539)
        self.assertEqual(numerics["PGM_CAP_BUNDLED_EXTENSIONS"], 65536)
        self.assertEqual(numerics["PGM_RESULT_TUPLES_CHUNK"], 5)
        self.assertNotIn("PGM_RESULT_OTHER", numerics)
        self.assertEqual(len(numerics), 128)

    def test_rejects_non_power_of_two_capability(self) -> None:
        policy = {
            "numeric_registry": [
                {
                    "group": "capability",
                    "bitmask": True,
                    "values": {"PGM_CAP_INVALID": 3},
                }
            ]
        }
        with self.assertRaisesRegex(
            check_c_api_contract.ApiContractError, "power of two"
        ):
            check_c_api_contract.flatten_numeric_registry(policy)

    def test_all_state_model_states_are_reachable(self) -> None:
        functions = {entry["name"] for entry in self.policy["functions"]}
        report = check_c_api_contract.validate_state_models(
            self.policy, functions
        )
        self.assertEqual(report["model_count"], 7)
        self.assertEqual(report["unreachable_state_count"], 0)

    def test_rejects_an_unreachable_state(self) -> None:
        policy = {
            "state_models": [
                {
                    "name": "sample",
                    "initial": "one",
                    "states": ["one", "two", "lost"],
                    "terminal": ["two", "lost"],
                    "transitions": [
                        {
                            "operation": "pgm_go",
                            "from": "one",
                            "to": "two",
                            "outcome": "ok",
                        }
                    ],
                }
            ]
        }
        with self.assertRaisesRegex(
            check_c_api_contract.ApiContractError, "unreachable states"
        ):
            check_c_api_contract.validate_state_models(policy, {"pgm_go"})

    def test_postgresql_19_protocol_inventory_has_zero_unknowns(self) -> None:
        functions = {entry["name"] for entry in self.policy["functions"]}
        numerics = check_c_api_contract.flatten_numeric_registry(self.policy)
        report = check_c_api_contract.validate_protocol(
            PROJECT_ROOT, self.protocol, functions, numerics
        )
        self.assertEqual(report["result_status_count"], 13)
        self.assertEqual(report["diagnostic_field_count"], 18)
        self.assertEqual(report["unclassified_upstream_results"], 0)

    def test_pre_v1_result_change_is_explicit(self) -> None:
        numerics = check_c_api_contract.flatten_numeric_registry(self.policy)
        report = check_c_api_contract.validate_pre_v1_changes(
            self.policy, numerics
        )
        self.assertEqual(report["removed_identifiers"], ["PGM_RESULT_OTHER"])

    def test_core_abi_v1_baseline_is_frozen_at_runtime_events(self) -> None:
        self.assertEqual(self.policy["abi"]["state"], "frozen")
        self.assertEqual(self.policy["abi"]["delivered_through"], "embedded-release")
        self.assertEqual(self.baseline["abi"]["frozen_through"], "runtime-events")
        self.assertEqual(len(self.baseline["functions"]), 63)
        self.assertEqual(len(self.baseline["numeric_values"]), 91)
        self.assertEqual(len(self.baseline["records"]), 16)
        self.assertEqual(len(self.baseline["opaque_records"]), 8)

    def test_additive_decisions_cannot_replace_a_frozen_decision(self) -> None:
        decisions = list(self.policy["frozen_decisions"])
        self.assertEqual(
            check_c_api_contract.validate_frozen_decisions(decisions),
            len(decisions),
        )
        decisions.remove("result-handle-is-the-bounded-chunk")
        decisions.append("replacement-decision-with-the-same-cardinality")
        with self.assertRaisesRegex(
            check_c_api_contract.ApiContractError,
            "preserve every frozen C API decision",
        ):
            check_c_api_contract.validate_frozen_decisions(decisions)

    @staticmethod
    def minimal_abi_fixture() -> tuple[dict, dict, dict, dict]:
        policy = {
            "abi": {
                "major": 1,
                "minor": 0,
                "state": "frozen",
                "delivered_through": "runtime-events",
                "core_freeze_after": "runtime-events",
            },
            "functions": [{"name": "pgm_go", "phase": "runtime-events"}],
        }
        catalog = {
            "functions": [
                {
                    "name": "pgm_go",
                    "function_type": (
                        "int (struct pgm_handle *, struct pgm_options *)"
                    ),
                }
            ],
            "records": [
                {"name": "pgm_handle", "complete": False},
                {
                    "name": "pgm_options",
                    "complete": True,
                    "size": 8,
                    "alignment": 4,
                    "fields": [
                        {
                            "name": "struct_size",
                            "canonical_type": "unsigned int",
                            "offset": 0,
                        },
                        {
                            "name": "flags",
                            "canonical_type": "unsigned int",
                            "offset": 4,
                        },
                    ],
                },
            ],
        }
        numerics = {
            "PGM_ABI_VERSION": 65536,
            "PGM_ABI_VERSION_MAJOR": 1,
            "PGM_ABI_VERSION_MINOR": 0,
            "PGM_STATUS_OK": 0,
        }
        baseline = {
            "postgresql_major": 19,
            "abi": {
                "major": 1,
                "minor": 0,
                "encoded_version": 65536,
                "state": "frozen",
                "frozen_through": "runtime-events",
                "symbol_version": "POSTGAMMA_1.0",
                "evolution": "additive-within-major",
            },
            "functions": [
                {
                    "name": "pgm_go",
                    "phase": "runtime-events",
                    "function_type": (
                        "int (struct pgm_handle *, struct pgm_options *)"
                    ),
                }
            ],
            "opaque_records": ["pgm_handle"],
            "numeric_values": dict(numerics),
            "records": [
                {
                    "name": "pgm_options",
                    "extensible": True,
                    "minimum_size": 8,
                    "alignment": 4,
                    "fields": copy.deepcopy(catalog["records"][1]["fields"]),
                }
            ],
        }
        return policy, catalog, numerics, baseline

    def test_abi_baseline_rejects_signature_change(self) -> None:
        policy, catalog, numerics, baseline = self.minimal_abi_fixture()
        catalog["functions"][0]["function_type"] = "void (void)"
        with self.assertRaisesRegex(
            check_c_api_contract.ApiContractError, "signature changed"
        ):
            check_c_api_contract.validate_abi_baseline(
                policy, catalog, numerics, baseline
            )

    def test_abi_baseline_rejects_numeric_change(self) -> None:
        policy, catalog, numerics, baseline = self.minimal_abi_fixture()
        numerics["PGM_STATUS_OK"] = 7
        with self.assertRaisesRegex(
            check_c_api_contract.ApiContractError, "numeric value changed"
        ):
            check_c_api_contract.validate_abi_baseline(
                policy, catalog, numerics, baseline
            )

    def test_abi_baseline_rejects_record_prefix_change(self) -> None:
        policy, catalog, numerics, baseline = self.minimal_abi_fixture()
        catalog["records"][1]["fields"][1]["offset"] = 8
        with self.assertRaisesRegex(
            check_c_api_contract.ApiContractError, "record prefix changed"
        ):
            check_c_api_contract.validate_abi_baseline(
                policy, catalog, numerics, baseline
            )

    def test_abi_baseline_allows_trailing_field_after_minor_increment(self) -> None:
        policy, catalog, numerics, baseline = self.minimal_abi_fixture()
        policy["abi"]["minor"] = 1
        numerics["PGM_ABI_VERSION"] = 65537
        numerics["PGM_ABI_VERSION_MINOR"] = 1
        catalog["records"][1]["size"] = 12
        catalog["records"][1]["fields"].append(
            {"name": "limit", "canonical_type": "unsigned int", "offset": 8}
        )
        report = check_c_api_contract.validate_abi_baseline(
            policy, catalog, numerics, baseline
        )
        self.assertEqual(report["compatible_trailing_field_count"], 1)


if __name__ == "__main__":
    unittest.main()
