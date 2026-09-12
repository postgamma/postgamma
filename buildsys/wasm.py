#!/usr/bin/env python3
"""Build and run isolated browser probes; this does not build the SQL kernel."""

from __future__ import annotations

import argparse
from contextlib import contextmanager
from functools import partial
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import re
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


PROBES = {
    "smoke": {"scope": "toolchain-smoke", "modules": {"smoke": ["smoke.c"]},
              "headers": [], "harness": ["smoke.html", "smoke-worker.mjs"]},
    "platform": {"scope": "abi-pthread-tls-stack-libc",
                 "modules": {"platform": ["platform.c", "platform-libc.c", "platform-threads.c"],
                             "stack-overflow": ["stack-overflow.c"]},
                 "headers": ["platform.h"],
                 "harness": ["platform.html", "platform-page.mjs", "platform-worker.mjs",
                             "stack-overflow-worker.mjs"]},
    "wait": {"scope": "wait-deadline-cancellation", "modules": {"wait": ["wait.c", "wait-clock.c"]},
             "headers": ["wait-clock.h"], "harness": ["wait.html", "wait-page.mjs", "wait-worker.mjs"]},
    "recovery": {"scope": "longjmp-error-recovery",
                 "modules": {"recovery": ["recovery.c", "recovery-jump.c"]},
                 "headers": ["recovery.h"],
                 "harness": ["recovery.html", "recovery-page.mjs", "recovery-worker.mjs"]},
    "opfs": {"scope": "opfs-semantics-and-proxy-cost",
             "modules": {"opfs": ["opfs.c", "opfs-bench.c"]},
             "headers": ["opfs.h"], "pre_js": ["opfs-observe.js"],
             "harness": ["opfs.html", "opfs-page.mjs", "opfs-worker.mjs"]},
    "deployment": {"scope": "browser-startup-and-deployment",
                   "modules": {"deployment": ["deployment.c"]}, "headers": [],
                   "harness": ["deployment.html", "deployment-page.mjs", "deployment-worker.mjs"]},
    "resource": {"scope": "provisional-thread-and-memory-capacity",
                 "modules": {"resource": ["resource.c"]}, "headers": [],
                 "harness": ["resource.html", "resource-page.mjs", "resource-worker.mjs"]},
}


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
def serve(directory: Path, page: str = "smoke.html"):
    server = ThreadingHTTPServer(("127.0.0.1", 0), partial(ProbeHandler, directory=str(directory)))
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        yield f"http://127.0.0.1:{server.server_port}/{page}"
    finally:
        server.shutdown()
        server.server_close()
        thread.join()


def run_browser(config: Configuration, tools: dict, env: dict, directory: Path,
                url: str | None = None, harness: Path | None = None, timeout_ms: int = 30000) -> dict:
    output = directory / "browser.json"
    directory.mkdir(parents=True, exist_ok=True)
    command = [tools["node"], str(harness or config.harness), "--root", str(config.root),
               "--output", str(output), "--profile", str(directory / "profile")]
    if config.browser_override:
        command += ["--executable", str(Path(config.browser_override).resolve())]
    if url:
        command += ["--url", url]
    if timeout_ms != 30000:
        command += ["--timeout-ms", str(timeout_ms)]
    launcher = config.deps / "browser-libs/launch-webkit.sh"
    if harness and launcher.is_file():
        command += ["--webkit-executable", str(launcher)]
    try:
        # Chromium's singleton socket cannot use long checkout/run paths.
        # Keep its disposable socket directory short and the profile in the run.
        with tempfile.TemporaryDirectory(prefix="pgm-wasm-", dir="/tmp") as temporary:
            run(command, directory / "browser.log",
                dict(browser_environment(config, env), TMPDIR=temporary),
                config.root, timeout=180 if harness else max(60, timeout_ms / 1000 + 30))
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


def build_configuration(config: Configuration, tools: dict, longjmp: str = "emscripten") -> dict:
    return {
        "profile": config.profile,
        "mode": config.mode,
        "longjmp": longjmp,
        "builder_sha256": digest(Path(__file__)),
        "resource_policy": read_json(config.root / "manifests/profiles/wasm-resource-budget.json"),
        "resource_builder_sha256": digest(config.root / "buildsys/wasm_budget.py"),
        "probe_layout": PROBES,
        "compiler": {key: tools[key] for key in (
            "emcc_version", "emcc_sha256", "clang_version", "clang_sha256",
            "sdk_config_sha256", "wasm_ld_sha256", "wasm_opt_sha256",
        )},
    }


def source_identity(config: Configuration) -> dict:
    root = config.root
    paths = [root / "Makefile", config.profile_path,
             root / "buildsys/wasm.py", root / "buildsys/wasm_toolchain.py",
             root / "buildsys/wasm_browsers.py",
             root / "buildsys/wasm_budget.py", root / "manifests/profiles/wasm-resource-budget.json",
             root / "buildsys/wasm.mk", root / "wasm/package.json",
             root / "wasm/package-lock.json", root / "postgres/src/include/utils/elog.h"]
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


def probe_sources(root: Path, name: str) -> dict:
    spec = PROBES[name]
    names = (spec["headers"] + spec.get("pre_js", []) +
             [source for sources in spec["modules"].values() for source in sources])
    result = {name: digest(root / "tests/wasm/probes" / name) for name in names}
    if name == "recovery":
        source = root / "postgres/src/include/utils/elog.h"
        if not source.is_file():
            raise WasmError("recovery probes require PostgreSQL source; run make source-init")
        result["postgres/src/include/utils/elog.h"] = digest(source)
    if name == "resource":
        from wasm_budget import POLICY
        result[POLICY] = digest(root / POLICY)
        result["buildsys/wasm_budget.py"] = digest(root / "buildsys/wasm_budget.py")
    return result


def recovery_macros(source: str) -> str:
    """Use the target's actual recovery macros without linking its SQL kernel."""
    match = re.search(r"^#define PG_TRY\(\.\.\.\).*?^extern PGDLLIMPORT sigjmp_buf \*PG_exception_stack;",
                      source, re.MULTILINE | re.DOTALL)
    if not match:
        raise WasmError("PostgreSQL recovery macro boundaries changed; review the probe extraction")
    block = match.group().split("\nextern PGDLLIMPORT", 1)[0]
    if not all(f"#define {name}(" in block for name in ("PG_CATCH", "PG_FINALLY", "PG_END_TRY", "PG_RE_THROW")):
        raise WasmError("PostgreSQL recovery macros are incomplete")
    return ("/* Extracted from postgres/src/include/utils/elog.h; PostgreSQL License.\n"
            " * See postgres/COPYRIGHT. Generated only for the recovery probe. */\n" + block + "\n")


def probe(config: Configuration, check: bool, name: str = "smoke", seed: int = 1,
          longjmp: str = "emscripten") -> Path:
    if not 1 <= seed <= 0xffffffff:
        raise WasmError("probe seed must be a nonzero uint32")
    if longjmp not in ("emscripten", "wasm"):
        raise WasmError("unsupported longjmp mode")
    spec = PROBES[name]
    tools, env, doctor_report = doctor(config)
    selected = build_configuration(config, tools, longjmp)
    config_id = fingerprint(selected)
    directory = config.build / "wasm" / config.target / config.mode / config_id
    inputs = {"schema_version": 1, "kind": "postgamma.wasm-probe-inputs",
              "configuration": selected, "config_id": config_id,
              "source": source_identity(config), "tools": tools,
              "runtime": {"seed": seed} if name == "wait" else {},
              "package_inputs": package_inputs(config)}
    write_json(directory / "inputs.json", inputs)
    current_run = directory / "runs" / run_id()
    current_run.mkdir(parents=True)
    report = {"schema_version": 1, "kind": "postgamma.wasm-probe-evidence",
              "status": "failed", "scope": spec["scope"], "probe": name,
              "kernel_validated": False, "browser_probe_validated": False,
              "inputs": inputs, "doctor_report": str(doctor_report)}
    try:
        site = directory / "probes" / name
        site.mkdir(parents=True, exist_ok=True)
        stamp = site / "build.json"
        expected = {"configuration": selected, "sources": probe_sources(config.root, name)}
        generated = {}
        resource_flags = []
        if name == "resource":
            from wasm_budget import probe_configuration
            header = site / "resource-policy.h"
            text, resource_flags = probe_configuration(config.root)
            header.write_text(text)
            generated[header.name] = digest(header)
        if name == "recovery":
            header = site / "recovery-pg-try.h"
            header.write_text(recovery_macros((config.root / "postgres/src/include/utils/elog.h").read_text()))
            generated[header.name] = digest(header)
        report["generated_headers"] = generated
        previous = read_json(stamp) if stamp.is_file() else {}
        reuse = outputs_match(previous, expected, site)
        if not reuse:
            print(f"wasm probe: compiling {config.target}/{config.mode}/{config_id}", flush=True)
            compiler = [sys.executable, tools["emcc"]]
            mode_flags = config.profile["modes"][config.mode]
            settings = config.profile.get("probes", {}).get(name, {})
            recovery_flags = [f"-sSUPPORT_LONGJMP={longjmp}"]
            if longjmp == "wasm":
                recovery_flags += ["-sWASM_LEGACY_EXCEPTIONS=0"]
            required = set()
            pre_js = [arg for filename in spec.get("pre_js", [])
                      for arg in ("--pre-js", str(config.root / "tests/wasm/probes" / filename))]
            for module, sources in spec["modules"].items():
                objects = []
                for filename in sources:
                    source = config.root / "tests/wasm/probes" / filename
                    obj = site / (source.stem + ".o")
                    run([*compiler, *config.profile["compile_flags"], *mode_flags["compile_flags"],
                         *recovery_flags, "-I", str(site),
                         "-c", str(source), "-o", str(obj)],
                        current_run / "build.log", env, config.root)
                    objects.append(str(obj))
                    required.add(obj.name)
                run([*compiler, *mode_flags["link_flags"], *config.profile["link_flags"],
                     *settings.get("link_flags", []),
                     *resource_flags,
                     *settings.get("module_link_flags", {}).get(module, []),
                     *recovery_flags, *pre_js,
                     *objects, "-o", str(site / (module + ".mjs"))],
                    current_run / "build.log", env, config.root)
                required.update({module + ".mjs", module + ".wasm"})
            outputs = {path.name: digest(path) for path in site.iterdir()
                       if path.name in required or
                       (path.suffix == ".map" and any(path.name.startswith(m + ".") for m in spec["modules"]))}
            if not required <= outputs.keys():
                raise WasmError("compiler did not produce all required probe assets")
            write_json(stamp, {"inputs": expected, "outputs": outputs})
        for filename in spec["harness"]:
            source_fixture = config.root / "tests/wasm/probes" / filename
            destination = site / filename
            if not destination.exists() or digest(destination) != digest(source_fixture):
                shutil.copyfile(source_fixture, destination)
        report["build_reused"] = reuse
        report["artifacts"] = read_json(stamp)["outputs"]
        report["harness"] = {filename: digest(site / filename) for filename in spec["harness"]}
        if check:
            if name == "deployment":
                setup_receipt = config.deps / "receipts/browser-matrix.json"
                report["browser_matrix_setup"] = ({"sha256": digest(setup_receipt),
                    "receipt": read_json(setup_receipt)} if setup_receipt.is_file() else {"provider": "system"})
            run([tools["node"], "--test", *(str(path) for path in
                 sorted((config.root / "tests/wasm/browser").glob("*.test.mjs")))],
                current_run / "harness-tests.log", env, config.root, timeout=30)
            report["harness_tests_passed"] = True
            page = name + ".html" + (f"?seed={seed}" if name == "wait" else
                                      f"?longjmp={longjmp}" if name == "recovery" else "")
            with serve(site, page) as url:
                if name == "deployment":
                    report["browser"] = run_browser(config, tools, env, current_run, url,
                                                   config.root / "tests/wasm/browser/deployment.mjs")
                else:
                    report["browser"] = run_browser(config, tools, env, current_run, url)
            report["browser_probe_validated"] = True
        report["status"] = "passed" if check else "built"
        print(f"wasm probe: {report['status']}; build {'reused' if reuse else 'compiled'}", flush=True)
    except Exception as error:
        report["error"] = str(error)
        if (current_run / "browser.json").is_file():
            report["browser"] = read_json(current_run / "browser.json")
        raise
    finally:
        write_json(current_run / "report.json", report)
        write_json(directory / "reports" / (name + ".json"),
                   {"run_report": str(current_run / "report.json"), "status": report["status"]})
        print(f"wasm probe report: {current_run / 'report.json'}", flush=True)
    return current_run / "report.json"


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
    parser.add_argument("command", choices=("setup", "browser-setup", "doctor", "probe-build", "probe-check", "resource-check", "clean", "kernel"))
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--build-dir", type=Path)
    parser.add_argument("--deps-dir", type=Path)
    parser.add_argument("--sdk-dir", type=Path)
    parser.add_argument("--profile", type=Path)
    parser.add_argument("--target", default="wasm32-emscripten-pthreads")
    parser.add_argument("--mode", choices=("debug", "release"), default="debug")
    parser.add_argument("--probe", choices=tuple(PROBES), default="smoke")
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--longjmp", choices=("emscripten", "wasm"), default="emscripten")
    parser.add_argument("--node", default="")
    parser.add_argument("--jobs", type=int, default=2)
    parser.add_argument("--browser-executable", default="")
    args = parser.parse_args()
    if args.command == "kernel":
        parser.exit(2, "wasm: kernel build and SQL checks are not implemented yet; "
                       "use make wasm-probe-check for the toolchain smoke probe\n")
    try:
        config = configuration(args)
        if args.command in ("setup", "browser-setup"):
            setup(config)
            if args.command == "browser-setup":
                from wasm_browsers import setup_browsers
                setup_browsers(config)
        elif args.command == "doctor":
            doctor(config)
        elif args.command == "clean":
            clean(config)
        elif args.command == "resource-check":
            from wasm_budget import check
            check(config, args.longjmp)
        else:
            probe(config, args.command == "probe-check", args.probe, args.seed, args.longjmp)
    except (WasmError, OSError, ValueError, subprocess.TimeoutExpired) as error:
        parser.exit(2, f"wasm: error: {error}\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
