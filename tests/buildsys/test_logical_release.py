"""Tests for the frozen logical management ABI v1.2 release baseline."""

from __future__ import annotations

import copy
import json
import sys
import unittest
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(PROJECT_ROOT / "buildsys"))

import check_c_api_contract  # noqa: E402
import check_logical_release  # noqa: E402


class LogicalReleaseTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.baseline = json.loads(
            (PROJECT_ROOT / "manifests/api/c-abi-v1-2.json").read_text(
                encoding="utf-8"
            )
        )
        cls.policy = json.loads(
            (PROJECT_ROOT / "manifests/api/c-public-api.json").read_text(
                encoding="utf-8"
            )
        )

    def test_freezes_exact_v1_2_identity(self) -> None:
        abi = check_logical_release.exact_abi_metadata(self.baseline)
        self.assertEqual(abi["encoded_version"], 65538)
        self.assertEqual(len(self.baseline["additive_functions"]), 4)
        self.assertEqual(len(self.baseline["additive_complete_records"]), 4)
        self.assertEqual(self.baseline["exported_symbol_count"], 73)

    def test_rejects_abi_identity_mutation(self) -> None:
        changed = copy.deepcopy(self.baseline)
        changed["abi"]["minor"] = 3
        with self.assertRaisesRegex(
            check_logical_release.LogicalReleaseCheckError,
            "metadata changed",
        ):
            check_logical_release.exact_abi_metadata(changed)

    def test_capability_mask_matches_the_live_policy(self) -> None:
        numerics = check_c_api_contract.flatten_numeric_registry(self.policy)
        public_api = {"marker": {"capabilities": "46079"}}
        observed = check_logical_release.validate_capabilities(
            self.baseline, numerics, public_api
        )
        self.assertEqual(observed, 46079)
        self.assertEqual(
            observed & numerics["PGM_CAP_PHYSICAL_BACKUP"],
            0,
        )

    def test_live_policy_preserves_frozen_logical_flags(self) -> None:
        core = json.loads((PROJECT_ROOT / "manifests/api/c-core-abi-v1.json").read_text())
        prior = json.loads((PROJECT_ROOT / "manifests/api/c-abi-v1-1.json").read_text())
        numerics = check_c_api_contract.flatten_numeric_registry(self.policy)
        closure = {"numeric_registry": {"values": numerics}}
        check_logical_release.validate_numerics(self.baseline, prior, core, closure)
        self.assertEqual(numerics["PGM_LOGICAL_SINGLE_TRANSACTION"], 64)
        for name, value in (("PGM_LOGICAL_FLAGS_ALL", 127), ("PGM_LOGICAL_CREATE", 64)):
            with self.subTest(name=name):
                changed = copy.deepcopy(closure)
                changed["numeric_registry"]["values"][name] = value
                with self.assertRaisesRegex(
                    check_logical_release.LogicalReleaseCheckError, "released numeric value changed"
                ):
                    check_logical_release.validate_numerics(self.baseline, prior, core, changed)

    def test_requires_all_process_safe_logical_tool_providers(self) -> None:
        report = {
            "status": "pass",
            "project_process_call_count": 42,
            "records": [
                {
                    "id": identifier,
                    "classification": "required_for_logical_management",
                    "actual_matches": 1,
                    "status": "classified",
                }
                for identifier in sorted(
                    check_logical_release.LOGICAL_PROCESS_PROVIDER_IDS
                )
            ],
        }
        values = check_logical_release.validate_process_assumptions(report)
        self.assertEqual(values["logical_management_provider_count"], 13)

        report["records"].pop()
        with self.assertRaisesRegex(
            check_logical_release.LogicalReleaseCheckError,
            "not inventoried",
        ):
            check_logical_release.validate_process_assumptions(report)

    def test_freezes_the_reviewed_private_libpq_namespace(self) -> None:
        expected = self.baseline["private_logical_tool"][
            "namespaced_libpq_symbols"
        ]
        report = {
            "kind": check_logical_release.LOGICAL_TOOL_LINK_KIND,
            "namespaced_symbols": {
                name: "postgamma_private_libpq_symbol_" + name
                for name in expected
            },
        }
        # Streaming logical tools track returned PGresults with libpq events.
        # Include their actual references independently of the baseline so an
        # omitted event entry cannot hide behind a self-consistent fixture.
        event_symbols = (
            "PQregisterEventProc",
            "PQresultInstanceData",
            "PQresultSetInstanceData",
        )
        report["namespaced_symbols"].update(
            (name, "postgamma_private_libpq_symbol_" + name)
            for name in event_symbols
        )
        self.assertEqual(
            check_logical_release.validate_logical_tool_namespace(
                self.baseline, report
            ),
            len(expected),
        )
        for name in event_symbols:
            for mutation in ("missing", "unprefixed"):
                with self.subTest(name=name, mutation=mutation):
                    changed = copy.deepcopy(report)
                    if mutation == "missing":
                        del changed["namespaced_symbols"][name]
                    else:
                        changed["namespaced_symbols"][name] = name
                    with self.assertRaisesRegex(
                        check_logical_release.LogicalReleaseCheckError,
                        "reviewed baseline",
                    ):
                        check_logical_release.validate_logical_tool_namespace(
                            self.baseline, changed
                        )
        for name in ("PQconnectdbParams", "PQunreviewed"):
            with self.subTest(extra=name):
                changed = copy.deepcopy(report)
                changed["namespaced_symbols"][name] = (
                    "postgamma_private_libpq_symbol_" + name
                )
                with self.assertRaisesRegex(
                    check_logical_release.LogicalReleaseCheckError,
                    "reviewed baseline",
                ):
                    check_logical_release.validate_logical_tool_namespace(
                        self.baseline, changed
                    )

    def test_rejects_socket_backed_cancel_symbols_from_namespace(self) -> None:
        changed = copy.deepcopy(self.baseline)
        expected = changed["private_logical_tool"][
            "namespaced_libpq_symbols"
        ]
        expected.append("PQgetCancel")
        expected.sort()
        report = {
            "kind": check_logical_release.LOGICAL_TOOL_LINK_KIND,
            "namespaced_symbols": {
                name: "postgamma_private_libpq_symbol_" + name
                for name in expected
            },
        }
        with self.assertRaisesRegex(
            check_logical_release.LogicalReleaseCheckError,
            "host-sensitive libpq entry points",
        ):
            check_logical_release.validate_logical_tool_namespace(changed, report)


if __name__ == "__main__":
    unittest.main()
