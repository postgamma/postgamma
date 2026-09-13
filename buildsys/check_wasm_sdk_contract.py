#!/usr/bin/env python3
"""Reject changes to the SDK sources used by the mailbox and proxy adapters."""
from pathlib import Path
from wasm_toolchain import WasmError, digest, read_json


def check(root: Path, sdk: Path, version: str) -> dict:
    path = root / 'manifests/profiles/wasm-sdk-contract.json'
    contract = read_json(path)
    if contract['schema_version'] != 1 or contract['emscripten_version'] != version:
        raise WasmError('SDK adapter contract requires reviewed Emscripten ' + contract['emscripten_version'])
    observed = {}
    for name, expected in contract['files'].items():
        source = sdk / 'upstream/emscripten' / name
        if not source.is_file() or digest(source) != expected:
            raise WasmError('SDK adapter source changed: ' + name + '; review mailbox/proxy behavior and revalidate the browser matrix before updating its contract')
        observed[name] = expected
    return {'version': version, 'contract_sha256': digest(path), 'files': observed}
