"""Keep upstream vector expectations and native fixture reuse explicit."""
import json
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "buildsys"))
from wasm_pgvector_fixtures import fixtures, l2_statements
from wasm_toolchain import WasmError


class VectorFixtureTests(unittest.TestCase):
    def test_native_and_upstream_results_are_adapted_without_executing_sql(self):
        with tempfile.TemporaryDirectory() as temporary:
            out = Path(temporary)
            report = fixtures(ROOT, out, out / "site")
            data = json.loads((out / "site/vector-fixtures.json").read_text())
            self.assertEqual(len(report["inputs"]), 5)
            self.assertEqual(len(data["nativeSetup"]), 7)
            for statements in data["upstream"].values():
                results = [s["rows"] for s in statements if "rows" in s]
                self.assertEqual(results, [[["[1,2,3]"], ["[1,2,4]"], ["[1,1,1]"], ["[0,0,0]"]], [["4"]], [["5"]], []])

    def test_changed_upstream_expected_shape_is_rejected(self):
        source = ROOT / "third_party/pgvector/test"
        sql = (source / "sql/hnsw_vector.sql").read_text()
        expected = (source / "expected/hnsw_vector.out").read_text()
        with self.assertRaises(WasmError):
            l2_statements(sql, expected.replace("(4 rows)", "(3 rows)", 1))
        with self.assertRaises(WasmError):
            l2_statements(sql, expected.replace("SELECT COUNT(*) FROM t;", "SELECT 99;", 1))


if __name__ == "__main__":
    unittest.main()
