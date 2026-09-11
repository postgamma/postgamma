#!/usr/bin/env python3
"""Build and run isolated browser probes; this does not build the SQL kernel."""

from __future__ import annotations

import argparse
from contextlib import contextmanager
from functools import partial
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import threading

from reset_directory import is_strict_child
from wasm_toolchain import (
    Configuration, WasmError, browser_description, browser_environment,
    capture, configuration, digest,
    fingerprint, package_inputs, package_ready, read_json, run, run_id, sdk_tools,
    setup, write_json, missing_browser_libraries,
)


class ProbeHandler(SimpleHTTPRequestHandler):
    extensions_map = {**SimpleHTTPRequestHandler.extensions_map,
                      ".mjs": "text/javascript", ".wasm": "application/wasm"}

    def end_headers(self) -> None:
        self.send_header("Cross-Origin-Opener-Policy", "same-origin")
        self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
        self.send_header("Cross-Origin-Resource-Policy", "same-origin")
        self.send_header("Cache-Control", "no-store")
        super().end_headers()

    def log_message(self, format: str, *args) -> None:
        pass


@contextmanager
def serve(directory: Path):
    server = ThreadingHTTPServer(("127.0.0.1", 0), partial(ProbeHandler, directory=str(directory)))
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        yield f"http://127.0.0.1:{server.server_port}/smoke.html"
    finally:
        server.shutdown()
        server.server_close()
        thread.join()


def run_browser(config: Configuration, tools: dict, env: dict, directory: Path,
                url: str | None = None) -> dict:
    output = directory / "browser.json"
    directory.mkdir(parents=True, exist_ok=True)
    command = [tools["node"], str(config.harness), "--root", str(config.root),
               "--output", str(output), "--profile", str(directory / "profile")]
    if config.browser_override:
        command += ["--executable", str(Path(config.browser_override).resolve())]
    if url:
        command += ["--url", url]
    try:
        # Chromium's singleton socket cannot use long checkout/run paths.
        # Keep its disposable socket directory short and the profile in the run.
        with tempfile.TemporaryDirectory(prefix="pgm-wasm-", dir="/tmp") as temporary:
            run(command, directory / "browser.log",
                dict(browser_environment(config, env), TMPDIR=temporary),
                config.root, timeout=60)
    except WasmError as error:
        if output.is_file():
            detail = read_json(output).get("error", str(error))
            raise WasmError(f"browser failed; report: {output}\n{detail}") from error
        raise
    result = read_json(output)
    if result.get("status") != "passed":
        raise WasmError(f"browser did not pass; see {output}")
    return result


def doctor(config: Configuration) -> tuple[dict, dict, Path]:
    directory = config.build / "wasm/doctor" / run_id()
    report = {"kind": "postgamma.wasm-doctor", "status": "failed",
              "scope": "Wasm probe tools; host AST generators are not required yet",
              "target": config.target, "mode": config.mode}
    try:
        tools, env = sdk_tools(config)
        report["tools"] = tools
        if not package_ready(config, tools, env):
            raise WasmError("locked browser dependencies are missing or changed; run make wasm-setup")
        description = browser_description(config, tools, env)
        if not Path(description["executable"]).is_file():
            raise WasmError("Chromium is missing; run make wasm-setup")
        missing = missing_browser_libraries(Path(description["executable"]), browser_environment(config, env))
        if missing:
            raise WasmError(f"missing Chromium libraries: {', '.join(missing)}; run make wasm-setup")
        report["browser"] = run_browser(config, tools, env, directory)
        libraries = config.deps / "browser-libs/receipt.json"
        report["browser_libraries"] = read_json(libraries) if libraries.is_file() else {"provider": "system"}
        report["status"] = "passed"
        print(f"wasm doctor: {tools['emcc_version']}; Node {tools['node_version']}; "
              f"Chromium {report['browser']['browserVersion']}", flush=True)
        return tools, env, directory / "report.json"
    except Exception as error:
        report["error"] = str(error)
        raise
    finally:
        write_json(directory / "report.json", report)
        print(f"wasm doctor report: {directory / 'report.json'}", flush=True)


def build_configuration(config: Configuration, tools: dict) -> dict:
    return {
        "profile": config.profile,
        "mode": config.mode,
        "compiler": {key: tools[key] for key in (
            "emcc_version", "emcc_sha256", "clang_version", "clang_sha256",
            "sdk_config_sha256", "wasm_ld_sha256", "wasm_opt_sha256",
        )},
    }


def source_identity(config: Configuration) -> dict:
    root = config.root
    paths = [root / "Makefile", config.profile_path,
             root / "buildsys/wasm.py", root / "buildsys/wasm_toolchain.py",
             root / "buildsys/wasm.mk", root / "wasm/package.json",
             root / "wasm/package-lock.json"]
    paths += sorted((root / "tests/wasm").rglob("*"))
    revisions = {}
    for name in ("postgres", "third_party/pgvector"):
        directory = root / name
        revisions[name] = {
            "commit": capture(["git", "-C", str(directory), "rev-parse", "HEAD"]),
            "status": capture(["git", "-C", str(directory), "status", "--porcelain"]),
        } if (directory / ".git").exists() else {"status": "not-materialized"}
    return {
        "commit": capture(["git", "-C", str(root), "rev-parse", "HEAD"]),
        "working_tree": capture(["git", "-C", str(root), "status", "--porcelain", "--untracked-files=all"]),
        "sources": {str(path.relative_to(root)) if path.is_relative_to(root) else str(path):
                    digest(path) for path in paths if path.is_file()},
        "upstream": revisions,
    }


def outputs_match(receipt: dict, expected: dict, directory: Path) -> bool:
    outputs = receipt.get("outputs", {})
    return (receipt.get("inputs") == expected and bool(outputs) and
            all((directory / name).is_file() and digest(directory / name) == value
                for name, value in outputs.items()))


def probe(config: Configuration, check: bool) -> None:
    tools, env, doctor_report = doctor(config)
    selected = build_configuration(config, tools)
    config_id = fingerprint(selected)
    directory = config.build / "wasm" / config.target / config.mode / config_id
    inputs = {"schema_version": 1, "kind": "postgamma.wasm-probe-inputs",
              "configuration": selected, "config_id": config_id,
              "source": source_identity(config), "tools": tools,
              "package_inputs": package_inputs(config)}
    write_json(directory / "inputs.json", inputs)
    current_run = directory / "runs" / run_id()
    current_run.mkdir(parents=True)
    report = {"schema_version": 1, "kind": "postgamma.wasm-probe-evidence",
              "status": "failed", "scope": "toolchain-smoke",
              "kernel_validated": False, "browser_probe_validated": False,
              "inputs": inputs, "doctor_report": str(doctor_report)}
    try:
        source = config.root / "tests/wasm/probes/smoke.c"
        site = directory / "probes/smoke"
        site.mkdir(parents=True, exist_ok=True)
        stamp = site / "build.json"
        expected = {"configuration": selected, "source_sha256": digest(source)}
        previous = read_json(stamp) if stamp.is_file() else {}
        reuse = outputs_match(previous, expected, site)
        if not reuse:
            print(f"wasm probe: compiling {config.target}/{config.mode}/{config_id}", flush=True)
            compiler = [sys.executable, tools["emcc"]]
            mode_flags = config.profile["modes"][config.mode]
            run([*compiler, *config.profile["compile_flags"], *mode_flags["compile_flags"],
                 "-c", str(source), "-o", str(site / "smoke.o")],
                current_run / "build.log", env, config.root)
            run([*compiler, *mode_flags["link_flags"], *config.profile["link_flags"],
                 str(site / "smoke.o"), "-o", str(site / "smoke.mjs")],
                current_run / "build.log", env, config.root)
            outputs = {path.name: digest(path) for path in site.iterdir()
                       if path.name.startswith("smoke.") and path.suffix in {".o", ".mjs", ".wasm", ".map"}}
            if not {"smoke.o", "smoke.mjs", "smoke.wasm"} <= outputs.keys():
                raise WasmError("compiler did not produce all required probe assets")
            write_json(stamp, {"inputs": expected, "outputs": outputs})
        for name in ("smoke.html", "smoke-worker.mjs"):
            source_fixture = config.root / "tests/wasm/probes" / name
            destination = site / name
            if not destination.exists() or digest(destination) != digest(source_fixture):
                shutil.copyfile(source_fixture, destination)
        report["build_reused"] = reuse
        report["artifacts"] = read_json(stamp)["outputs"]
        report["harness"] = {name: digest(site / name) for name in ("smoke.html", "smoke-worker.mjs")}
        if check:
            with serve(site) as url:
                report["browser"] = run_browser(config, tools, env, current_run, url)
            report["browser_probe_validated"] = True
        report["status"] = "passed" if check else "built"
        print(f"wasm probe: {report['status']}; build {'reused' if reuse else 'compiled'}", flush=True)
    except Exception as error:
        report["error"] = str(error)
        raise
    finally:
        write_json(current_run / "report.json", report)
        write_json(directory / "reports/smoke.json", {"run_report": str(current_run / "report.json"),
                                                       "status": report["status"]})
        print(f"wasm probe report: {current_run / 'report.json'}", flush=True)


def clean(config: Configuration) -> None:
    target = (config.build / "wasm").resolve()
    if not is_strict_child(target, config.build):
        raise WasmError(f"refusing to clean a Wasm output outside BUILD_DIR: {target}")
    run([sys.executable, str(config.root / "buildsys/reset_directory.py"),
         "--path", str(target), "--within", str(config.build), "--remove-only"],
        config.deps / "receipts" / ("clean-" + run_id() + ".log"), timeout=60)
    print(f"wasm clean: {target}; installed dependencies retained")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("setup", "doctor", "probe-build", "probe-check", "clean", "kernel"))
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--build-dir", type=Path)
    parser.add_argument("--deps-dir", type=Path)
    parser.add_argument("--sdk-dir", type=Path)
    parser.add_argument("--profile", type=Path)
    parser.add_argument("--target", default="wasm32-emscripten-pthreads")
    parser.add_argument("--mode", choices=("debug", "release"), default="debug")
    parser.add_argument("--node", default="")
    parser.add_argument("--jobs", type=int, default=2)
    parser.add_argument("--browser-executable", default="")
    args = parser.parse_args()
    if args.command == "kernel":
        parser.exit(2, "wasm: kernel build and SQL checks are not implemented yet; "
                       "use make wasm-probe-check for the toolchain smoke probe\n")
    try:
        config = configuration(args)
        if args.command == "setup":
            setup(config)
        elif args.command == "doctor":
            doctor(config)
        elif args.command == "clean":
            clean(config)
        else:
            probe(config, args.command == "probe-check")
    except (WasmError, OSError, ValueError, subprocess.TimeoutExpired) as error:
        parser.exit(2, f"wasm: error: {error}\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
