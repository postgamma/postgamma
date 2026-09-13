#!/usr/bin/env python3
"""Inspect a linked module using the pinned SDK's WebAssembly reader."""

from __future__ import annotations

import argparse
from pathlib import Path
import sys

from wasm_toolchain import WasmError, digest, write_json


PROVIDERS = {
    "opfs-owner": "postgamma_storage_dispatch postgamma_storage_listen postgamma_storage_notify "
        "postgamma_storage_unlisten emscripten_unwind_to_js_event_loop",
    "shared-memory": "memory emscripten_get_heap_max emscripten_resize_heap",
    "recovery-and-fatal-diagnostics": "exit proc_exit __assert_fail __cxa_throw _abort_js "
        "_emscripten_throw_longjmp __handle_stack_overflow emscripten_err emscripten_out",
    "environment-and-clock": "environ_sizes_get environ_get _tzset_js clock_time_get "
        "emscripten_get_now emscripten_date_now _mktime_js _localtime_js _gmtime_js",
    "pthread-lifecycle": "_emscripten_thread_set_strongref emscripten_exit_with_live_runtime "
        "_emscripten_init_main_thread_js _emscripten_thread_mailbox_await "
        "_emscripten_receive_on_main_thread_js emscripten_check_blocking_allowed "
        "__pthread_create_js _emscripten_thread_cleanup _emscripten_runtime_keepalive_clear "
        "emscripten_num_logical_cores _emscripten_notify_mailbox_postmessage",
    "wasmfs": "_wasmfs_get_preloaded_file_size _wasmfs_copy_preloaded_file_data "
        "_wasmfs_stdin_get_char _wasmfs_get_num_preloaded_files _wasmfs_get_num_preloaded_dirs "
        "_wasmfs_get_preloaded_parent_path _wasmfs_get_preloaded_child_path "
        "_wasmfs_get_preloaded_path_name _wasmfs_get_preloaded_file_mode "
        "_wasmfs_jsimpl_alloc_file _wasmfs_jsimpl_get_size _wasmfs_jsimpl_read "
        "_wasmfs_jsimpl_write _wasmfs_jsimpl_set_size _wasmfs_jsimpl_free_file",
    "web-crypto": "random_get postgamma_storage_channel_name",
    # HBA/libpq retain network address formatting and resolution branches. The
    # private memory connection entry does not expose host/port/socket options.
    "retained-address-compatibility": "getaddrinfo getnameinfo",
    # libc raise/abort use SDK signal dispatch; role interrupts use the registry.
    "sdk-local-signal-dispatch": "__call_sighandler",
}


def provider(name: str) -> str:
    import re
    if re.fullmatch(r"invoke_[vifdj]+", name):
        return "same-thread-longjmp"
    for group, names in PROVIDERS.items():
        if name in names.split():
            return group
    raise WasmError("unreviewed linked Wasm import: " + name)


def inspect(sdk: Path, wasm: Path) -> dict:
    sys.path.insert(0, str(sdk / "upstream/emscripten"))
    from tools.webassembly import ExternType, Module, OpCode
    with Module(str(wasm)) as module:
        required = {"pgm_instance_open", "pgm_instance_close", "pgm_connection_open",
                    "pgm_connection_close", "pgm_execute", "pgm_result_value", "pgm_result_free",
                    "pgm_error_message", "pgm_error_free"}
        exports = {entry.name for entry in module.get_exports()}
        if not required.issubset(exports):
            raise WasmError("kernel entry points were removed from the final module")
        imports = []
        for entry in module.get_imports():
            if entry.module not in ("env", "wasi_snapshot_preview1"):
                raise WasmError("unexpected import namespace: " + entry.module)
            imports.append({"module": entry.module, "name": entry.field,
                            "kind": entry.kind.name, "provider": provider(entry.field)})
        measured = {}
        for entry in module.get_exports():
            if entry.kind == ExternType.GLOBAL and entry.name in (
                    "__tls_size", "__tls_align", "__data_end", "__heap_base"):
                value = module.get_global(entry.index)
                if value.mutable or value.init[0][0] != OpCode.I32_CONST:
                    raise WasmError("expected immutable linked layout global: " + entry.name)
                measured[entry.name] = value.init[0][1][0]
        if set(measured) != {"__tls_size", "__tls_align", "__data_end", "__heap_base"}:
            raise WasmError("linked layout evidence is incomplete")
    return {"kind": "postgamma.wasm-link-audit", "status": "passed",
            "module_sha256": digest(wasm), "imports": imports, "layout": measured,
            "kernel_exports": sorted(required),
            "reader_sha256": digest(sdk / "upstream/emscripten/tools/webassembly.py"),
            "native_network_supported": False, "native_processes_supported": False,
            "dynamic_extensions_supported": False}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sdk", required=True, type=Path)
    parser.add_argument("--wasm", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    write_json(args.output, inspect(args.sdk, args.wasm))


if __name__ == "__main__":
    main()
