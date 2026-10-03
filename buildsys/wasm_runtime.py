#!/usr/bin/env python3
"""Exercise shared runtime implementations with their Wasm providers."""
from __future__ import annotations

import argparse
from pathlib import Path
import shutil

from wasm import run_browser, serve
from wasm_toolchain import (WasmError, configuration, digest, fingerprint, run,
                            run_id, sdk_tools, write_json, read_json)


def target_layouts(config, longjmp):
    # Use this suite's verified target layouts. A release-only checkout must not
    # depend on a prior debug build or accept its stale generation receipt.
    from wasm_kernel import prepare
    directory, _, _, _ = prepare(config, longjmp)
    receipt = directory / "reports/generate.json"
    if not receipt.is_file():
        raise WasmError("run make wasm-kernel-generate before the filesystem runtime check")
    latest = read_json(receipt)
    report = read_json(Path(latest["run_report"]))
    if latest["status"] != "passed" or report["status"] != "passed":
        raise WasmError("target layout generation has not passed")
    names = ("backend_state_layout.h", "backend_state_descriptors.inc", "guc_state_layout.h")
    files = {}
    for name in names:
        path = directory / "generated/runtime/include/postgamma" / name
        relative = str(path.relative_to(directory))
        if digest(path) != report["outputs"][relative]:
            raise WasmError("target layout differs from generation receipt: " + name)
        files[str(path)] = digest(path)
    return directory / "generated/runtime/include", files


def check(config, longjmp: str) -> Path:
    tools, env = sdk_tools(config)
    layouts, layout_files = target_layouts(config, longjmp)
    sdk_headers = config.sdk / "upstream/emscripten/system/lib/wasmfs"
    sources = ["runtime/platform/emscripten/locale.c", "tests/wasm/runtime/libc.c", "runtime/platform/emscripten/filesystem.cpp", "embedded-c/src/data_directory_lock.c",
               "runtime/src/path_runtime.c", "runtime/src/execution_context.c", "runtime/src/backend_state_runtime.c", "runtime/src/virtual_signal.c",
               "tests/wasm/runtime/filesystem.c", "runtime/src/thread_runtime.c", "runtime/src/backend_execution_runtime.c", "runtime/platform/emscripten/thread.c", "runtime/platform/emscripten/io.c", "runtime/platform/emscripten/shmem.c",
               "runtime/platform/emscripten/dsm.c", "runtime/platform/emscripten/semaphore.c",
               "tests/wasm/runtime/threads.c", "tests/wasm/runtime/wait.c", "tests/wasm/runtime/execution.c", "tests/wasm/runtime/clock.c", "tests/wasm/runtime/shmem.c", "tests/wasm/runtime/dsm.c"]
    inputs = {"tools": tools, "mode": config.mode, "longjmp": longjmp,
              "layouts": layout_files,
              "sdk_headers": {str(p.relative_to(sdk_headers)): digest(p) for p in sorted(sdk_headers.rglob("*.h"))},
              "files": {name: digest(config.root / name) for name in [*sources,
                  "tests/runtime/test_path_runtime.c", "runtime/include/postgamma/file_platform.h",
                  "embedded-c/include/postgamma/private/data_directory_lock.h",
                  *(str(p.relative_to(config.root)) for p in sorted((config.root / "runtime/include").rglob("*.h"))),
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
        # Compile the PostgreSQL timer wrappers with the shared timer module;
        # the fixture supplies only clock and current-session dependencies.
        backend = (config.root / "runtime/src/postgres_backend_runtime.c").read_text()
        begin = backend.index("long\npostgamma_backend_adjust_wait_timeout")
        end = backend.index("\n\nbool\npostgamma_postgres_quantum_begin", begin)
        (site / "timer-wait.inc").write_text(backend[begin:end] + "\n")
        flags = ["-D_GNU_SOURCE", "-Wall", "-Wextra", "-Werror", "-Wno-error=pthreads-mem-growth", "-pthread",
                 *config.profile["modes"][config.mode]["compile_flags"],
                 "-I" + str(config.root / "runtime/include"), "-I" + str(site),
                 "-I" + str(config.root / "embedded-c/include"), "-I" + str(layouts),
                 f"-sSUPPORT_LONGJMP={longjmp}"]
        objects = []
        for index, name in enumerate(sources):
            cpp = name.endswith(".cpp")
            obj = current / f"object-{index}.o"
            run([str(config.sdk / "upstream/emscripten" / ("em++" if cpp else "emcc")),
                 "-std=c++17" if cpp else "-std=c11", *flags,
                 *(["-isystem", str(sdk_headers)] if cpp else []),
                 "-c", str(config.root / name), "-o", str(obj)], current / "build.log", env, config.root)
            objects.append(str(obj))
        report["objects"] = {name: digest(Path(obj)) for name, obj in zip(sources, objects)}
        run([str(config.sdk / "upstream/emscripten/em++"), *flags, *objects,
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
