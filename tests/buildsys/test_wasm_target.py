"""Keep target selection tied to complete, current resource evidence."""
import copy
import os
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "buildsys"))
import check_wasm_target as target_check
from wasm_toolchain import WasmError, digest, read_json, write_json


class WasmTargetTests(unittest.TestCase):
    def fixture(self, root):
        target = read_json(ROOT / target_check.TARGET)
        policy = read_json(ROOT / target_check.POLICY)
        write_json(root / target_check.POLICY, policy)
        profile = read_json(ROOT / target["probe_profile"])
        write_json(root / target["probe_profile"], profile)
        source = root / "tests/wasm/probes/checked.c"
        source.parent.mkdir(parents=True)
        source.write_text("unchanged probe")
        llvm = root / "tools/llvm"
        binaryen = root / "tools/binaryen"
        tools = {"emcc": str(root / "tools/emcc"), "sdk": str(root / "tools"),
                 "settings": {"LLVM_ROOT": str(llvm), "BINARYEN_ROOT": str(binaryen)}}
        for name, path in {"emcc": root / "tools/emcc", "clang": llvm / "clang", "wasm_ld": llvm / "wasm-ld",
                           "wasm_opt": binaryen / "bin/wasm-opt", "sdk_config": root / "tools/.emscripten"}.items():
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(name)
            tools[name + "_sha256"] = digest(path)
        budget = {"kind": "postgamma.wasm-resource-budget", "status": "passed", "policy": policy,
                  "policy_sha256": digest(root / target_check.POLICY), "measured_abi": {"pointer": 4},
                  "decision": {"memory64_triggered": False, "first_kernel_target": target["target"],
                               "emscripten_longjmp": "emscripten", "first_storage_profile": "ephemeral-wasmfs"},
                  "evidence": {}}
        directory = root / "build/config"
        for name in sorted(target_check.REQUIRED):
            site = directory / "probes" / name
            site.mkdir(parents=True)
            artifact = site / (name + ".wasm")
            artifact.write_bytes(b"probe module")
            path = directory / "runs" / name / "report.json"
            proof = {"status": "passed", "probe": name, "browser_probe_validated": True,
                     "inputs": {"configuration": {"profile": profile}, "tools": tools,
                                "source": {"sources": {str(source.relative_to(root)): digest(source)}}},
                     "artifacts": {artifact.name: digest(artifact)}, "harness": {}}
            write_json(path, proof)
            write_json(directory / "reports" / (name + ".json"), {"status": "passed", "run_report": str(path)})
            budget["evidence"][name] = {"path": str(path), "sha256": digest(path)}
        return target, budget

    def test_valid_selection_does_not_claim_memory64_or_kernel_execution(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            target, budget = self.fixture(root)
            result = target_check.validate(root, target, budget)
            self.assertEqual(result["memory64"]["status"], "not-applicable")
            self.assertFalse(result["memory64"]["probes_executed"])
            self.assertFalse(result["kernel_validated"])

    def test_latest_failed_budget_does_not_fall_back_to_older_success(self):
        with tempfile.TemporaryDirectory() as temporary:
            build = Path(temporary)
            base = build / "wasm/wasm32-emscripten-pthreads/debug"
            old, new = [base / name / "reports/resource-budget.json" for name in ("old", "new")]
            write_json(old, {"status": "passed"})
            write_json(new, {"status": "failed"})
            os.utime(old, ns=(1000, 1000))
            os.utime(new, ns=(2000, 2000))
            self.assertEqual(target_check.latest_budget(build, "wasm32-emscripten-pthreads", "debug"), new)

    def test_changed_source_or_compiled_artifact_invalidates_selection(self):
        for changed in ("tests/wasm/probes/checked.c", "build/config/probes/resource/resource.wasm"):
            with tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                target, budget = self.fixture(root)
                (root / changed).write_bytes(b"changed input")
                with self.assertRaisesRegex(WasmError, "changed"):
                    target_check.validate(root, target, budget)

    def test_memory64_trigger_and_claimed_execution_require_new_review(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            target, budget = self.fixture(root)
            altered = copy.deepcopy(budget)
            altered["decision"]["memory64_triggered"] = True
            with self.assertRaisesRegex(WasmError, "Memory64"):
                target_check.validate(root, target, altered)
            target["memory64"]["probes_executed"] = True
            with self.assertRaisesRegex(WasmError, "Memory64"):
                target_check.validate(root, target, budget)

    def test_a_newer_individual_failure_invalidates_an_older_complete_budget(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            target, budget = self.fixture(root)
            write_json(root / "build/config/reports/resource.json", {"status": "failed", "run_report": "new attempt"})
            with self.assertRaisesRegex(WasmError, "newer attempt"):
                target_check.validate(root, target, budget)

    def test_policy_change_and_incomplete_evidence_are_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            target, budget = self.fixture(root)
            incomplete = copy.deepcopy(budget)
            del incomplete["evidence"]["opfs"]
            with self.assertRaisesRegex(WasmError, "incomplete"):
                target_check.validate(root, target, incomplete)
            budget["policy_sha256"] = "stale policy"
            with self.assertRaisesRegex(WasmError, "policy changed"):
                target_check.validate(root, target, budget)

    def test_latest_snapshot_must_match_its_saved_run_report(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            run, latest = root / "run.json", root / "latest.json"
            write_json(run, {"status": "failed"})
            write_json(latest, {"status": "failed", "run_report": str(run)})
            document, reference = target_check.load_budget(latest)
            self.assertEqual(reference["path"], str(run))
            self.assertEqual(document["status"], "failed")
            write_json(latest, {"status": "passed", "run_report": str(run)})
            with self.assertRaisesRegex(WasmError, "differs"):
                target_check.load_budget(latest)


if __name__ == "__main__":
    unittest.main()
