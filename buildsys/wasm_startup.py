#!/usr/bin/env python3
"""Build or exercise the private browser kernel startup bridge."""

from __future__ import annotations

import argparse
from pathlib import Path
from threading import Event

from wasm import run_browser, serve
from wasm_link import Builder
from wasm_toolchain import WasmError, configuration, digest, read_json, run, write_json


def validate_lifecycle(browser: dict, pool: int) -> dict:
    probe = browser["probe"]
    if (probe.get("rounds") != 3 or probe.get("runningWorkers") != 0 or
            probe.get("unusedWorkers") != pool or probe.get("ticks", 0) < 2 or
            probe.get("pageTicks", 0) < 2 or not probe.get("workerPoolDisposed")):
        raise WasmError("incomplete lifecycle, Worker return, or control responsiveness evidence")
    roles, stacks = [], []
    for line in probe["output"]:
        if "POSTGAMMA_RUNTIME " in line:
            fields = dict(item.split("=", 1) for item in line.split("POSTGAMMA_RUNTIME ", 1)[1].split())
            for name in ("role_process_launches", "forbidden_process_launch_attempts", "unsupported_role_requests",
                         "role_threads_active", "runnable_sessions", "running_quantums", "execution_tokens_active"):
                if fields.get(name) != "0":
                    raise WasmError("kernel lifecycle accounting failed: " + name)
            if fields.get("provider") != "pooled" or int(fields["dedicated_threads_started"]) < 3:
                raise WasmError("required threaded kernel roles were not observed")
            roles.append(fields)
        if "POSTGAMMA_STACK " in line:
            fields = dict(item.split("=", 1) for item in line.split("POSTGAMMA_STACK ", 1)[1].split())
            for name in ("observation_failures", "depth_validation_failures", "accounting_failures", "active_reservation_bytes"):
                if fields.get(name) != "0":
                    raise WasmError("kernel stack accounting failed: " + name)
            if fields["class"] in ("dedicated", "client") and int(fields["depth_validations"]) < 1:
                raise WasmError("kernel stack depth limit was not validated")
            stacks.append(fields)
    if len(roles) != 3 or len(stacks) != 9 or len({row["generation"] for row in stacks}) != 3:
        raise WasmError("missing per-instance role or stack evidence")
    return {"roles": roles, "stacks": stacks, "running_workers": 0,
            "returned_workers": pool, "linear_memory_bytes": probe["memoryBytes"],
            "scope": "three sequential lifecycles; no concurrent SQL or durable storage claim"}


def startup(config, longjmp: str, action: str) -> Path:
    builder = Builder(config, longjmp)
    report = {"kind": "postgamma.wasm-kernel-startup", "status": "failed",
              "sql_validated": False, "action": action}
    try:
        builder.backend()
        builder.frontends()
        builder.modules()
        builder.product()
        builder.link(config.root / "wasm/bridge/kernel_smoke.c")
        site = builder.output / "site"
        report["site"] = str(site)
        sources = [(f"wasm/src/{name}/kernel.ts", f"src/{name}/kernel.js")
                   for name in ("bindings", "loader", "worker")]
        sources.append(("wasm/src/loader/assets.ts", "src/loader/assets.js"))
        for source, target in sources:
            run([builder.tools["node"], str(config.root / "buildsys/wasm_typescript.mjs"),
                 str(config.root / source), str(site / target)],
                builder.current / "typescript.log", builder.env, config.root)
        (site / "kernel.html").write_bytes((config.root / "tests/wasm/kernel/kernel.html").read_bytes())
        from wasm_assets import generate as asset_manifest
        report["asset_manifest"] = asset_manifest(builder, "src/worker/kernel.js")
        report["link_report"] = str(builder.current / "report.json")
        report["harness"] = {name: digest(site / name)
                             for name in (*[target for _, target in sources], "kernel.html")}
        if action == "check":
            with serve(site, "kernel.html") as url:
                report["browser"] = run_browser(config, builder.tools, builder.env, builder.current, url)
            policy = read_json(config.root / "manifests/profiles/wasm-resource-budget.json")
            pool = sum(role["count"] for role in policy["thread_roles"])
            report["lifecycle"] = validate_lifecycle(report["browser"], pool)
            report["sql_validated"] = True
        report["status"] = "passed"
    except Exception as error:
        report["error"] = str(error)
        raise
    finally:
        builder.finish()
        path = builder.current / "startup.json"
        write_json(path, report)
        write_json(builder.directory / "reports/startup.json", {
            "status": report["status"], "sql_validated": report["sql_validated"],
            "run_report": str(path)})
        print(f"wasm startup: {report['status']}; {path}", flush=True)
    return path


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("build", "check", "serve"))
    parser.add_argument("--root", required=True, type=Path)
    for name in ("build-dir", "deps-dir", "sdk-dir", "profile"):
        parser.add_argument("--" + name, type=Path)
    parser.add_argument("--target", default="wasm32-emscripten-pthreads")
    parser.add_argument("--mode", default="debug", choices=("debug", "release"))
    parser.add_argument("--longjmp", default="emscripten")
    parser.add_argument("--node", default="")
    parser.add_argument("--browser-executable", default="")
    parser.add_argument("--jobs", type=int, default=2)
    args = parser.parse_args()
    try:
        path = startup(configuration(args), args.longjmp, args.action)
        if args.action == "serve":
            with serve(Path(read_json(path)["site"]), "kernel.html") as url:
                print(f"Kernel startup fixture: {url} (Ctrl-C to stop)", flush=True)
                Event().wait()
    except KeyboardInterrupt:
        pass
    except Exception as error:
        parser.exit(2, "wasm startup: " + str(error).splitlines()[0] + "\n")


if __name__ == "__main__":
    main()
