"""Check resource accounting and refusal to promote incomplete evidence."""
import copy
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "buildsys"))
import wasm
import wasm_budget as budget
import wasm_toolchain as toolchain


class ResourceBudgetTests(unittest.TestCase):
    def policy(self):
        return toolchain.read_json(ROOT / budget.POLICY)

    def test_shared_heap_is_counted_once_and_idle_and_reserve_stacks_are_included(self):
        policy = self.policy()
        plan = budget.plan(policy)
        mib = 1024 * 1024
        self.assertEqual((plan["pthread_count"], plan["worker_count"]), (15, 16))
        self.assertEqual(plan["stack_bytes"], 90 * mib)
        self.assertEqual(plan["tls_reserve_bytes"], 16 * mib)
        self.assertEqual(plan["estimated_linear_peak_bytes"], 386 * mib)
        self.assertEqual(plan["maximum_headroom_bytes"], 126 * mib)
        policy["outside_linear_memory"]["estimated_bytes"] *= 2
        self.assertEqual(plan, budget.plan(policy))

    def test_unbounded_roles_and_infeasible_envelopes_fail(self):
        for change in (lambda p: p["thread_roles"][0].update(count=-1),
                       lambda p: p["thread_roles"][0].update(count=True),
                       lambda p: p["thread_roles"].pop(),
                       lambda p: p.update(maximum_memory_bytes=128 * 1024**2),
                       lambda p: p.update(initial_max_live_sessions=20)):
            policy = self.policy()
            change(policy)
            with self.assertRaises(toolchain.WasmError):
                budget.plan(policy)

    def test_generated_probe_tracks_policy_changes(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            policy = self.policy()
            toolchain.write_json(root / budget.POLICY, policy)
            before, flags = budget.probe_configuration(root)
            self.assertIn("-sPTHREAD_POOL_SIZE=15", flags)
            policy["thread_roles"][-1]["count"] += 1
            toolchain.write_json(root / budget.POLICY, policy)
            after, flags = budget.probe_configuration(root)
            self.assertNotEqual(before, after)
            self.assertIn("-sPTHREAD_POOL_SIZE=16", flags)

    def test_a_failed_or_build_only_dependency_cannot_close_the_budget(self):
        policy = self.policy()
        evidence = {name: {"status": "passed", "browser_probe_validated": True}
                    for name in ("platform", "wait", "recovery", "opfs", "deployment", "resource")}
        for name in evidence:
            changed = copy.deepcopy(evidence)
            changed[name]["browser_probe_validated"] = False
            with self.assertRaisesRegex(toolchain.WasmError, name):
                budget.summarize(policy, changed)

    def test_failure_is_published_instead_of_retaining_an_older_success(self):
        with tempfile.TemporaryDirectory() as temporary:
            profile = ROOT / "manifests/profiles/wasm-emscripten.json"
            config = toolchain.Configuration(ROOT, Path(temporary), ROOT / ".deps/wasm", ROOT / ".deps/wasm/emsdk/test",
                                             profile, toolchain.read_json(profile), "debug", False)
            selected = {"configuration": "failure-test"}
            directory = config.build / "wasm" / config.target / config.mode / toolchain.fingerprint(selected)
            latest = directory / "reports/resource-budget.json"
            toolchain.write_json(latest, {"status": "passed", "run_report": "old success"})
            with mock.patch.object(toolchain, "sdk_tools", return_value=({}, {})), \
                 mock.patch.object(wasm, "build_configuration", return_value=selected), \
                 mock.patch.object(wasm, "source_identity", return_value={}), \
                 mock.patch.object(wasm, "probe", side_effect=toolchain.WasmError("injected failure")):
                with self.assertRaisesRegex(toolchain.WasmError, "injected failure"):
                    budget.check(config, "emscripten")
            result = toolchain.read_json(latest)
            self.assertEqual(result["status"], "failed")
            self.assertEqual(toolchain.read_json(Path(result["run_report"]))["error"], "injected failure")


if __name__ == "__main__":
    unittest.main()
