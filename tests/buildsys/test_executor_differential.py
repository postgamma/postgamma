"""Tests for pooled/dedicated executor differential validation."""

from __future__ import annotations

import sys
import unittest
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(PROJECT_ROOT / "buildsys"))

import check_executor_differential  # noqa: E402


class ExecutorDifferentialTests(unittest.TestCase):
    def test_runtime_distinguishes_provider_models(self) -> None:
        pooled = check_executor_differential.parse_runtime(
            "POSTGAMMA_RUNTIME provider=pooled client_quantums=8 "
            "quantum_yields=7 carrier_migrations=6 role_threads_active=0",
            "pooled",
        )
        dedicated = check_executor_differential.parse_runtime(
            "POSTGAMMA_RUNTIME provider=dedicated client_quantums=0 "
            "quantum_yields=0 carrier_migrations=0 role_threads_active=0",
            "dedicated",
        )
        self.assertEqual(pooled["carrier_migrations"], 6)
        self.assertEqual(dedicated["carrier_migrations"], 0)

    def test_rejects_dedicated_quantum_use(self) -> None:
        with self.assertRaisesRegex(
            check_executor_differential.ExecutorDifferentialError,
            "dedicated Oracle",
        ):
            check_executor_differential.parse_runtime(
                "POSTGAMMA_RUNTIME provider=dedicated client_quantums=1 "
                "quantum_yields=0 carrier_migrations=0 role_threads_active=0",
                "dedicated",
            )


if __name__ == "__main__":
    unittest.main()
