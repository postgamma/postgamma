#!/usr/bin/env python3
"""Exercise shared runtime implementations with their Wasm providers."""
from __future__ import annotations

import argparse
from pathlib import Path
import shutil

from wasm import run_browser, serve
from wasm_toolchain import (WasmError, configuration, digest, fingerprint, run,
                            run_id, sdk_tools, write_json)


def check(config, longjmp: str) -> Path:
    tools, env = sdk_tools(config)
    sources = ["runtime/src/thread_runtime.c", "runtime/src/backend_execution_runtime.c", "runtime/platform/emscripten/thread.c", "runtime/platform/emscripten/io.c", "runtime/platform/emscripten/shmem.c",
               "runtime/platform/emscripten/dsm.c", "runtime/platform/emscripten/semaphore.c",
               "tests/wasm/runtime/threads.c", "tests/wasm/runtime/wait.c", "tests/wasm/runtime/execution.c", "tests/wasm/runtime/clock.c", "tests/wasm/runtime/shmem.c", "tests/wasm/runtime/dsm.c"]
    inputs = {"tools": tools, "mode": config.mode, "longjmp": longjmp,
              "files": {name: digest(config.root / name) for name in [*sources,
                  "runtime/include/postgamma/thread_runtime.h",
                  "runtime/include/postgamma/thread_platform.h", "runtime/include/postgamma/io_platform.h",
                  "runtime/include/postgamma/io_platform_redirect.h", "buildsys/wasm_runtime.py",
                  "runtime/include/postgamma/backend_execution_runtime.h", "runtime/include/postgamma/shmem_platform.h",
                  "runtime/include/postgamma/dsm_platform.h", "runtime/include/postgamma/semaphore_platform.h",
                  "tests/runtime/test_backend_execution_runtime.c", "runtime/src/postgres_backend_runtime.c",
                  "tests/wasm/runtime/worker.mjs", "tests/wasm/runtime/runtime.html"]}}
    directory = config.build / "wasm" / config.target / config.mode / "runtime" / fingerprint(inputs)
    current = directory / "runs" / run_id()
    site = directory / "site"
    site.mkdir(parents=True, exist_ok=True)
    report = {"kind": "postgamma.wasm-runtime", "status": "failed", "inputs": inputs,
              "kernel_validated": False, "sql_validated": False}
    try:
        # Compile the shared timer implementation, with only its clock/context dependencies supplied by the fixture.
        backend = (config.root / "runtime/src/postgres_backend_runtime.c").read_text()
        begin = backend.index("typedef struct PostgammaVirtualTimer")
        end = backend.index("} PostgammaVirtualTimer;", begin) + len("} PostgammaVirtualTimer;")
        (site / "timer-type.inc").write_text(backend[begin:end] + "\n")
        begin = backend.index("long\npostgamma_backend_adjust_wait_timeout")
        end = backend.index("\n\nbool\npostgamma_postgres_quantum_begin", begin)
        (site / "timer-wait.inc").write_text(backend[begin:end] + "\n")
        run([str(config.sdk / "upstream/emscripten/emcc"),
             "-std=c11", "-D_GNU_SOURCE", "-Wall", "-Wextra", "-Werror", "-Wno-error=pthreads-mem-growth", "-pthread",
             *config.profile["modes"][config.mode]["compile_flags"],
             "-I" + str(config.root / "runtime/include"), "-I" + str(site),
             *(str(config.root / name) for name in sources),
             "--no-entry", "-sMODULARIZE=1", "-sEXPORT_ES6=1", "-sENVIRONMENT=worker",
             "-sEXPORTED_RUNTIME_METHODS=[\"HEAPU8\"]", "-sPTHREAD_POOL_SIZE=4",
             "-sPTHREAD_POOL_SIZE_STRICT=2", "-sALLOW_BLOCKING_ON_MAIN_THREAD=0",
             "-sINITIAL_MEMORY=67108864", "-sMAXIMUM_MEMORY=134217728",
             "-sALLOW_MEMORY_GROWTH=1", "-sABORTING_MALLOC=0", "-sSTACK_SIZE=2097152",
             "-sDEFAULT_PTHREAD_STACK_SIZE=2097152", "-sSTACK_OVERFLOW_CHECK=2",
             "-sWASMFS=1", "-sASSERTIONS=2", f"-sSUPPORT_LONGJMP={longjmp}",
             "-o", str(site / "runtime.mjs")], current / "build.log", env, config.root)
        for name in ("runtime.html", "worker.mjs"):
            shutil.copy2(config.root / "tests/wasm/runtime" / name, site / name)
        report["outputs"] = {p.name: digest(p) for p in site.iterdir() if p.is_file()}
        with serve(site, "runtime.html") as url:
            report["browser"] = run_browser(config, tools, env, current, url)
        if report["browser"]["probe"]["completed"] != 48:
            raise WasmError("runtime test did not complete every role and TLS destructor")
        report["status"] = "passed"
    except Exception as error:
        report["error"] = str(error)
        raise
    finally:
        write_json(current / "report.json", report)
        print(f"wasm runtime: {report['status']}; {current / 'report.json'}", flush=True)
    return current / "report.json"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
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
        check(configuration(args), args.longjmp)
    except (WasmError, OSError, ValueError) as error:
        parser.exit(2, f"wasm runtime: {error}\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
