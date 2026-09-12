#!/usr/bin/env python3
"""Validate the initial kernel target against the latest resource evidence."""
import argparse
from pathlib import Path

from wasm_budget import POLICY, plan
from wasm_toolchain import WasmError, digest, read_json, run_id, write_json

TARGET = "manifests/profiles/wasm-kernel.json"
REQUIRED = {"platform", "wait", "recovery", "opfs", "deployment", "resource"}


def latest_budget(build: Path, target: str, mode: str) -> Path:
    reports = list((build / "wasm" / target / mode).glob("*/reports/resource-budget.json"))
    if not reports:
        raise WasmError("resource evidence is missing; run make wasm-resource-check")
    # An incomplete recent attempt must not silently fall back to older success.
    return max(reports, key=lambda p: p.stat().st_mtime_ns)


def load_budget(path: Path) -> tuple[dict, dict]:
    latest = read_json(path)
    run = Path(latest["run_report"])
    document = read_json(run)
    if {k: v for k, v in latest.items() if k != "run_report"} != document:
        raise WasmError("latest budget differs from its saved run report")
    return document, {"path": str(run), "sha256": digest(run),
                      "latest_path": str(path), "latest_sha256": digest(path)}


def validate(root: Path, target: dict, budget: dict) -> dict:
    policy = read_json(root / POLICY)
    layout = plan(policy)
    if (target.get("schema_version") != 1 or target.get("kind") != "postgamma.wasm-kernel-target" or
            target.get("status") != "selected-for-initial-port" or target.get("resource_policy") != POLICY):
        raise WasmError("unsupported initial kernel target profile")
    if budget.get("kind") != "postgamma.wasm-resource-budget" or budget.get("status") != "passed":
        raise WasmError("latest resource budget did not pass; run make wasm-resource-check")
    if budget.get("policy_sha256") != digest(root / POLICY) or budget.get("policy") != policy:
        raise WasmError("resource policy changed; run make wasm-resource-check")
    decision = budget["decision"]
    if (decision.get("memory64_triggered") is not False or target.get("memory64", {}).get("status") != "not-applicable" or
            target["memory64"].get("probes_executed") is not False):
        raise WasmError("Memory64 trigger requires a new target-width review, not an unmeasured pass")
    if (target["target"] != policy["target"] or target["target"] != decision["first_kernel_target"] or
            target["pointer_bits"] != 32 or target["pointer_bits"] != budget["measured_abi"]["pointer"] * 8 or
            target["longjmp"] != decision["emscripten_longjmp"] or target["filesystem"] != "wasmfs" or
            target["storage_profile"] != decision["first_storage_profile"]):
        raise WasmError("kernel target differs from the measured resource decision")
    if set(budget.get("evidence", {})) != REQUIRED:
        raise WasmError("resource evidence set is incomplete")
    checked = {}

    def verify(path: Path, checksum: str):
        if str(path) not in checked:
            checked[str(path)] = digest(path)
        if checked[str(path)] != checksum:
            raise WasmError(f"resource input or artifact changed: {path}; run make wasm-resource-check")

    profile = read_json(root / target["probe_profile"])
    for name, entry in budget["evidence"].items():
        path = Path(entry["path"])
        verify(path, entry["sha256"])
        proof = read_json(path)
        if proof.get("status") != "passed" or proof.get("browser_probe_validated") is not True or proof.get("probe") != name:
            raise WasmError(f"invalid {name} browser evidence")
        latest = read_json(path.parents[2] / "reports" / (name + ".json"))
        if latest.get("status") != "passed" or latest.get("run_report") != str(path):
            raise WasmError(f"{name} has a newer attempt; refresh the complete resource budget")
        if proof["inputs"]["configuration"]["profile"] != profile:
            raise WasmError("SDK/probe profile differs from the resource evidence")
        for name, checksum in proof["inputs"]["source"]["sources"].items():
            # Entry-point wiring can change without changing the tested driver,
            # flags, C/JS sources or policy. Those actual inputs remain required.
            if name not in ("Makefile", "buildsys/wasm.mk"):
                verify(root / name, checksum)
        site = path.parents[2] / "probes" / proof["probe"]
        for name, checksum in {**proof["artifacts"], **proof["harness"]}.items():
            verify(site / name, checksum)
        tools = proof["inputs"]["tools"]
        llvm = Path(tools["settings"]["LLVM_ROOT"])
        for name, location in {"emcc": Path(tools["emcc"]), "clang": llvm / "clang",
                               "wasm_ld": llvm / "wasm-ld", "sdk_config": Path(tools["sdk"]) / ".emscripten",
                               "wasm_opt": Path(tools["settings"]["BINARYEN_ROOT"]) / "bin/wasm-opt"}.items():
            verify(location, tools[name + "_sha256"])
    return {"target": target, "emsdk": profile["emsdk"], "resource_policy": policy,
            "pthread_pool_size": layout["pthread_count"], "verified_files": len(checked),
            "memory64": target["memory64"], "kernel_compiled": False, "kernel_validated": False}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--build-dir", type=Path)
    parser.add_argument("--mode", choices=("debug", "release"), default="debug")
    parser.add_argument("--target", default="wasm32-emscripten-pthreads")
    parser.add_argument("--longjmp", default="emscripten")
    args = parser.parse_args()
    root = args.root.resolve()
    build = (args.build_dir or root / "build").resolve()
    output = build / "wasm/target-selection" / run_id() / "report.json"
    report = {"schema_version": 1, "kind": "postgamma.wasm-target-selection", "status": "failed"}
    path = None
    try:
        target = read_json(root / TARGET)
        if args.target != target["target"] or args.longjmp != target["longjmp"]:
            raise WasmError("requested target or longjmp mode differs from the selected initial kernel profile")
        path = latest_budget(build, target["target"], args.mode)
        document, report["budget"] = load_budget(path)
        report["target_profile_sha256"] = digest(root / TARGET)
        report["validator_sha256"] = digest(Path(__file__))
        report.update(validate(root, target, document))
        report["status"] = "passed"
        print("wasm target: wasm32 selected for the initial port; Memory64 comparison not applicable")
        return 0
    except (WasmError, OSError, ValueError, KeyError) as error:
        report["error"] = str(error)
        print(f"wasm target: {error}")
        return 2
    finally:
        write_json(output, report)
        if path is not None:
            write_json(path.parent / "target-selection.json", dict(report, run_report=str(output)))
        write_json(build / "wasm/target-selection" / (args.mode + ".json"), dict(report, run_report=str(output)))
        print(f"wasm target report: {output}")


if __name__ == "__main__":
    raise SystemExit(main())
