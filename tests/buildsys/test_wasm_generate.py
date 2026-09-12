"""Keep target availability separate from reviewed ownership."""
import copy
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "buildsys"))
from wasm_generate import ast_cached, project_policy
from wasm_toolchain import WasmError


class TargetStatePolicyTests(unittest.TestCase):
    def setUp(self):
        self.policy = {"decisions": [
            {"id": "external:session", "owner": "session", "rationale": "session ownership"},
            {"id": "internal:contrib/sample/module.c:module.c::cache", "owner": "role", "rationale": "role ownership"},
        ]}
        self.catalog = {"candidates": [{"id": "external:session"}]}

    def test_explicit_availability_preserves_every_shared_owner(self):
        original = copy.deepcopy(self.policy)
        projected, exclusions = project_policy(self.policy, self.catalog,
            {"definition_prefixes": {"contrib/": "module excluded from target"}})
        self.assertEqual(self.policy, original)
        self.assertEqual([e["owner"] for e in projected["decisions"]], ["session", "role"])
        self.assertEqual(len(exclusions), 1)
        self.assertEqual(projected["decisions"][0], original["decisions"][0])
        self.assertEqual(projected["decisions"][1]["availability"], "conditional")

    def test_missing_required_state_is_not_implicitly_dropped(self):
        with self.assertRaisesRegex(WasmError, "unreviewed target policy exclusions"):
            project_policy(self.policy, self.catalog, {})

    def test_new_target_state_requires_shared_ownership_review(self):
        self.catalog["candidates"].append({"id": "external:new_state"})
        with self.assertRaisesRegex(WasmError, "shared ownership review"):
            project_policy(self.policy, self.catalog, {})

    def test_external_symbols_cannot_be_excluded_by_a_path_prefix(self):
        with self.assertRaisesRegex(WasmError, "external:session"):
            project_policy(self.policy, {"candidates": []},
                           {"definition_prefixes": {"contrib/": "excluded", "session": "invalid"}})

    def test_ast_reuse_rejects_changed_inputs_missing_outputs_and_failed_attempts(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            outputs = [root / "source.json", root / "generated.json"]
            command, inputs = ["ast-tool"], {"target_headers": "v1"}

            def succeed(*args):
                for path in outputs:
                    path.write_text('{"result":"generated"}')

            with mock.patch("wasm_generate.run", side_effect=succeed) as run:
                ast_cached(command, outputs, inputs, root / "log", {}, root)
                ast_cached(command, outputs, inputs, root / "log", {}, root)
                self.assertEqual(run.call_count, 1)
                outputs[1].unlink()
                ast_cached(command, outputs, inputs, root / "log", {}, root)
                self.assertEqual(run.call_count, 2)
                ast_cached(command, outputs, {"target_headers": "v2"}, root / "log", {}, root)
                self.assertEqual(run.call_count, 3)
                outputs[0].write_text("changed layout")
                ast_cached(command, outputs, {"target_headers": "v2"}, root / "log", {}, root)
                self.assertEqual(run.call_count, 4)
            with mock.patch("wasm_generate.run", side_effect=WasmError("failed compiler")):
                with self.assertRaises(WasmError):
                    ast_cached(command, outputs, inputs, root / "log", {}, root)
            with mock.patch("wasm_generate.run", side_effect=succeed) as run:
                ast_cached(command, outputs, inputs, root / "log", {}, root)
                self.assertEqual(run.call_count, 1)


if __name__ == "__main__":
    unittest.main()
