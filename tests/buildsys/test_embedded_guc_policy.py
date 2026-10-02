"""Tests for adapter-owned embedded GUC defaults and safety settings."""

from __future__ import annotations

import copy
import sys
import unittest
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(PROJECT_ROOT / "buildsys"))

import generate_embedded_guc_policy  # noqa: E402
import postgresql_embedded_adapter  # noqa: E402


class EmbeddedGucPolicyTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.adapter = postgresql_embedded_adapter.load_adapter(
            PROJECT_ROOT / "manifests/postgresql/embedded.json"
        )

    def test_policy_is_generated_from_the_major_adapter(self) -> None:
        content = generate_embedded_guc_policy.emit_policy(self.adapter)
        self.assertEqual(len(self.adapter["guc_policy"]["defaults"]), 14)
        self.assertEqual(len(self.adapter["guc_policy"]["safety"]), 18)
        self.assertIn('{"shared_buffers", "16MB"}', content)
        self.assertIn('{"restart_after_crash", "off"}', content)
        self.assertIn("#ifdef USE_BONJOUR", content)

    def test_runtime_has_no_handwritten_policy_table(self) -> None:
        runtime = (
            PROJECT_ROOT
            / "runtime/include/postgamma/postgres_postmaster_runtime_impl.h"
        ).read_text(encoding="utf-8")
        self.assertIn('#include "postgamma/embedded_guc_policy.inc"', runtime)
        self.assertNotIn('{"shared_buffers", "16MB"}', runtime)

    def test_adapter_rejects_duplicate_policy_names(self) -> None:
        document = copy.deepcopy(self.adapter)
        document["guc_policy"]["defaults"].append(
            copy.deepcopy(document["guc_policy"]["defaults"][0])
        )
        with self.assertRaisesRegex(
            postgresql_embedded_adapter.EmbeddedAdapterError,
            "invalid or duplicated",
        ):
            postgresql_embedded_adapter.validate_adapter(document)


if __name__ == "__main__":
    unittest.main()
