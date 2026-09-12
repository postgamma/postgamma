#!/usr/bin/env python3
"""Configure and capture the isolated PostgreSQL WebAssembly target."""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys

from configure_postgres import CONFIGURE_ENVIRONMENT
from wasm_kernel_checks import configured_abi, validate_database
from wasm_toolchain import (
    WasmError, capture, configuration, digest, fingerprint, read_json, run,
    run_id, sdk_tools, write_json,
)


def clean_environment(environment: dict) -> dict:
    result = dict(environment)
    for name in (*CONFIGURE_ENVIRONMENT, "NM", "RANLIB", "ZIC"):
        if name not in ("PATH", "PERL", "PYTHON"):
            result.pop(name, None)
    for name in list(result):
        if name.startswith(("ac_cv_", "pgac_cv_", "POSTGAMMA_")):
            result.pop(name)
    result.update(LC_ALL="C", CONFIG_SITE="/dev/null", PKG_CONFIG="false",
                  PKG_CONFIG_LIBDIR="", PKG_CONFIG_PATH="")
    return result


def configure(source: Path, build: Path, arguments: list[str], env: dict,
              log: Path) -> None:
    if (build / "config.status").is_file():
        return
    build.mkdir(parents=True, exist_ok=True)
    run([str(source / "configure"), *arguments], log, env, build)


def prepare(config, longjmp: str) -> tuple[Path, dict, dict, dict]:
    profile = read_json(config.root / "manifests/profiles/wasm-kernel.json")
    if longjmp != profile["longjmp"] or config.target != profile["target"]:
        raise WasmError("kernel target must match the selected profile")
    tools, sdk_env = sdk_tools(config)
    source = config.root / "postgres"
    if capture(["git", "-C", str(source), "status", "--porcelain"]):
        raise WasmError("PostgreSQL source must be clean for a target build")
    inputs = {
        "kind": "postgamma.wasm-kernel-inputs", "schema_version": 1,
        "target": config.target, "mode": config.mode, "profile": profile,
        "postgres_commit": capture(["git", "-C", str(source), "rev-parse", "HEAD"]),
        "tools": tools,
        "driver_sources": {name: digest(config.root / name) for name in (
            "buildsys/wasm_kernel.py", "buildsys/wasm_cc_capture.py",
            "buildsys/cc_capture.py", "buildsys/merge_compile_db.py",
            "buildsys/wasm_toolchain.py", "buildsys/wasm_kernel_checks.py")},
    }
    directory = config.build / "wasm" / config.target / config.mode / fingerprint(inputs)
    write_json(directory / "inputs.json", inputs)
    env = clean_environment(sdk_env)
    sdk_bin = config.sdk / "upstream/emscripten"
    llvm_bin = Path(tools["settings"]["LLVM_ROOT"])
    flags = profile["compile_flags"] + profile["modes"][config.mode]
    env.update(CC=str(sdk_bin / "emcc"), CXX=str(sdk_bin / "em++"),
               AR=str(sdk_bin / "emar"), RANLIB=str(sdk_bin / "emranlib"),
               NM=str(llvm_bin / "llvm-nm"), CFLAGS=shlex.join(flags),
               CXXFLAGS=shlex.join(flags), CPPFLAGS=shlex.join(profile["cppflags"]),
               LDFLAGS="-pthread -sSUPPORT_LONGJMP=" + longjmp)
    return directory, inputs, tools, env


def host_generators(config, inputs: dict, log: Path) -> Path:
    # zic is the only compiled generator executed by this profile. Build it
    # from the same PostgreSQL revision, with its own host configuration.
    env = clean_environment(dict(os.environ))
    env.update(CC="cc", CXX="c++", CFLAGS="-O2", CXXFLAGS="-O2")
    identity = {"postgres_commit": inputs["postgres_commit"],
                "cc": capture(["cc", "--version"]).splitlines()[0],
                "configure": inputs["profile"]["configure"],
                "environment": {k: env[k] for k in CONFIGURE_ENVIRONMENT if k in env}}
    directory = config.build / "wasm/host-tools" / fingerprint(identity)
    build = directory / "postgres-build"
    configure(config.root / "postgres", build, identity["configure"], env, log)
    run(["make", "-j", str(config.jobs), "-C", str(build / "src/backend"),
         "generated-headers"], log, env)
    run(["make", "-C", str(build / "src/port"), "pg_config_paths.h"], log, env)
    run(["make", "-j", str(config.jobs), "-C", str(build / "src/timezone"), "zic"], log, env)
    executable = build / "src/timezone/zic"
    write_json(directory / "inputs.json", identity)
    write_json(directory / "generators.json", {
        "zic": {"path": str(executable), "sha256": digest(executable),
                "version": capture([str(executable), "--version"], env)},
        "target_executables_run": [],
    })
    return executable


def build_target(config, command: str, longjmp: str) -> Path:
    directory, inputs, tools, env = prepare(config, longjmp)
    current = directory / "runs" / (command + "-" + run_id())
    log = current / "build.log"
    report = {"kind": "postgamma.wasm-kernel-build", "schema_version": 1,
              "task": command, "status": "failed", "kernel_linked": False,
              "sql_validated": False, "target_executables_run": [],
              "inputs_sha256": digest(directory / "inputs.json")}
    try:
        zic = host_generators(config, inputs, log)
        env["ZIC"] = str(zic)
        build = directory / "configure"
        arguments = ["--build=" + capture(["sh", str(config.root / "postgres/config/config.guess")]),
                     "--host=wasm32-unknown-emscripten", "--with-template=linux",
                     "--prefix=/postgamma", *inputs["profile"]["configure"],
                     *inputs["profile"]["target_configure"]]
        if config.mode == "debug":
            arguments += ["--enable-cassert"]
        configure(config.root / "postgres", build, arguments, env, log)
        run(["make", "-j", str(config.jobs), "-C", str(build / "src/backend"),
             "generated-headers"], log, env)
        run(["make", "-C", str(build / "src/port"), "pg_config_paths.h"], log, env)
        report["configure"] = {"arguments": arguments,
                               "pg_config_sha256": digest(build / "src/include/pg_config.h"),
                               "log": str(build / "config.log"),
                               "host_zic": str(zic)}
        report["abi"] = configured_abi(build / "src/include/pg_config.h")
        report["configure"]["cache_answers"] = dict(re.findall(
            r"^([a-zA-Z0-9_]+_cv_[a-zA-Z0-9_]+)=(.*)$",
            (build / "config.log").read_text(), re.M))
        report["configure"]["decisions"] = inputs["profile"]["configure_decisions"]
        report["generated_headers"] = {name: digest(build / name) for name in (
            "src/include/pg_config.h", "src/include/utils/errcodes.h",
            "src/include/catalog/pg_type_d.h", "src/include/utils/fmgroids.h",
            "src/include/nodes/nodetags.h", "src/port/pg_config_paths.h")}
        if command == "capture":
            database = directory / "compile-db"
            env.update(EM_COMPILER_WRAPPER=str(config.root / "buildsys/wasm_cc_capture.py"),
                       POSTGAMMA_CAPTURE_DIR=str(database / "fragments"))
            for subdir, targets in inputs["profile"]["capture_targets"].items():
                run(["make", "-j", str(config.jobs), "-C", str(build / subdir),
                     *targets], log, env)
            run([sys.executable, str(config.root / "buildsys/merge_compile_db.py"),
                 "--fragments", str(database / "fragments"),
                 "--output", str(database / "compile_commands.json"),
                 "--source-root", str(config.root / "postgres"),
                 "--generated-root", str(build)], log, env)
            report["compile_database"] = str(database / "compile_commands.json")
            report["capture"] = validate_database(
                database / "compile_commands.json", config.root / "postgres", build,
                Path(tools["settings"]["LLVM_ROOT"]) / "clang",
                Path(tools["settings"]["CACHE"]) / "sysroot")
        report["status"] = "passed"
    except Exception as error:
        report["error"] = str(error)
        raise
    finally:
        write_json(current / "report.json", report)
        write_json(directory / "reports" / (command + ".json"),
                   {"status": report["status"], "run_report": str(current / "report.json")})
        print(f"wasm kernel {command}: {report['status']}; {current / 'report.json'}", flush=True)
    return directory


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("configure", "capture"))
    parser.add_argument("--root", type=Path, required=True)
    for option in ("build-dir", "deps-dir", "sdk-dir", "profile"):
        parser.add_argument("--" + option, type=Path)
    parser.add_argument("--target", default="wasm32-emscripten-pthreads")
    parser.add_argument("--mode", choices=("debug", "release"), default="debug")
    parser.add_argument("--longjmp", default="emscripten")
    parser.add_argument("--node", default="")
    parser.add_argument("--browser-executable", default="")
    parser.add_argument("--jobs", type=int, default=2)
    args = parser.parse_args()
    try:
        build_target(configuration(args), args.command, args.longjmp)
    except (WasmError, OSError, ValueError, subprocess.TimeoutExpired) as error:
        parser.exit(2, f"wasm kernel: {error}\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
