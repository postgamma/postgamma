from __future__ import annotations

import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "buildsys"))

from check_pgvector_contract import (  # noqa: E402
    PgvectorContractError,
    expected_symbols,
    parallel_worker_symbols,
    validate_kernel_macro_bridges,
)


class PgvectorContractTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        (self.root / "src").mkdir()

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def test_symbol_closure_includes_non_sql_parallel_worker_entries(self) -> None:
        (self.root / "src" / "build.c").write_text(
            'CreateParallelContext("vector", "FixtureParallelMain", workers);\n',
            encoding="utf-8",
        )
        sql = (
            "CREATE FUNCTION vector_dims(vector) RETURNS integer "
            "AS 'MODULE_PATHNAME', 'vector_dims' LANGUAGE C;\n"
        )
        symbols = expected_symbols(sql, self.root)
        self.assertEqual(
            symbols["FixtureParallelMain"],
            "postgamma_module_pgvector_FixtureParallelMain",
        )
        self.assertEqual(
            symbols["vector_dims"], "postgamma_module_pgvector_vector_dims"
        )
        self.assertEqual(
            symbols["pg_finfo_vector_dims"],
            "postgamma_module_pgvector_finfo_vector_dims",
        )

    def test_rejects_a_source_tree_without_parallel_worker_entries(self) -> None:
        (self.root / "src" / "build.c").write_text(
            "int ordinary_function(void) { return 0; }\n", encoding="utf-8"
        )
        with self.assertRaisesRegex(
            PgvectorContractError, "no parallel worker entries"
        ):
            parallel_worker_symbols(self.root)

    @staticmethod
    def backend_runtime() -> dict[str, object]:
        return {
            "schema_version": 1,
            "kind": "postgamma.backend-state-runtime",
            "slots": [
                {
                    "id": "external:MainLWLockArray",
                    "owner": "role",
                    "name": "MainLWLockArray",
                    "enum": "POSTGAMMA_BACKEND_STATE_MAIN_LWLOCK_ARRAY",
                }
            ],
        }

    def write_macro_bridge_fixture(self, definition: str) -> Path:
        (self.root / "src" / "hnsw.c").write_text(
            "void acquire(void) { use(AddinShmemInitLock); }\n"
            "void release(void) { use(AddinShmemInitLock); }\n",
            encoding="utf-8",
        )
        adapter = self.root / "adapter.h"
        adapter.write_text(
            '#include "storage/lwlock.h"\n' + definition + "\n",
            encoding="utf-8",
        )
        return adapter

    def test_accepts_reviewed_kernel_macro_bridge(self) -> None:
        adapter = self.write_macro_bridge_fixture(
            "#define MainLWLockArray "
            "POSTGAMMA_BACKEND_STATE_GLOBAL_MainLWLockArray"
        )
        bridges = validate_kernel_macro_bridges(
            self.root, adapter, self.backend_runtime()
        )
        self.assertEqual(len(bridges), 1)
        self.assertEqual(bridges[0]["observed_source_use_count"], 2)

    def test_rejects_missing_kernel_macro_bridge(self) -> None:
        adapter = self.write_macro_bridge_fixture("/* missing bridge */")
        with self.assertRaisesRegex(PgvectorContractError, "bridge is missing"):
            validate_kernel_macro_bridges(
                self.root, adapter, self.backend_runtime()
            )


if __name__ == "__main__":
    unittest.main()
