"""Tests for the embedded release instance-owned state reachability gate."""

from __future__ import annotations

import sys
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "buildsys"))

from check_instance_reachability import (  # noqa: E402
    InstanceReachabilityError,
    validate_reachability,
)


def documents() -> tuple[dict[str, object], ...]:
    catalog = {
        "kind": "postgamma.backend-mutable-state-catalog",
        "candidates": [
            {
                "id": "instance:a",
                "name": "a",
                "definition_path": "a.c",
                "translation_units": ["a.c"],
                "use_count": 3,
            }
        ],
    }
    policy = {
        "kind": "postgamma.backend-state-ownership",
        "decisions": [{"id": "instance:a", "owner": "instance"}],
    }
    runtime = {
        "kind": "postgamma.backend-state-runtime",
        "owner_slots": {"instance": 1},
        "owner_bytes": {"instance": 8},
        "slots": [
            {"id": "instance:a", "owner": "instance", "offset": 0, "size": 8}
        ],
    }
    state = {
        "bytes": 8,
        "slots": 1,
        "slot_ids": ["instance:a"],
        "reviewed_policy_decisions": 1,
    }
    multi = {
        "kind": "postgamma.multi-instance",
        "status": "pass",
        "marker": {"instances_live_peak": "2"},
        "instance_state": state,
    }
    return catalog, policy, runtime, multi


class InstanceReachabilityTests(unittest.TestCase):
    def test_accepts_exact_instance_closure(self) -> None:
        report = validate_reachability(*documents())
        self.assertEqual(report["instance_slots"], 1)
        self.assertEqual(report["instance_use_count"], 3)
        self.assertTrue(report["runtime_exact"])

    def test_rejects_unreviewed_catalog_state(self) -> None:
        catalog, policy, runtime, multi = documents()
        catalog["candidates"].append(
            {
                "id": "role:b",
                "name": "b",
                "definition_path": "b.c",
                "translation_units": ["b.c"],
                "use_count": 1,
            }
        )
        with self.assertRaisesRegex(InstanceReachabilityError, "unreviewed"):
            validate_reachability(catalog, policy, runtime, multi)

    def test_rejects_runtime_layout_drift(self) -> None:
        catalog, policy, runtime, multi = documents()
        multi["instance_state"] = {
            **multi["instance_state"],
            "bytes": 16,
        }
        with self.assertRaisesRegex(InstanceReachabilityError, "different"):
            validate_reachability(catalog, policy, runtime, multi)


if __name__ == "__main__":
    unittest.main()
