#!/usr/bin/env python3
"""Generate and verify the C/TypeScript storage operation and errno contract."""
import argparse
import json
from pathlib import Path
import re
from wasm_toolchain import WasmError, digest, read_json

MANIFEST = 'manifests/wasm-storage-protocol.json'
HEADER = 'runtime/include/postgamma/wasm_storage_protocol.h'
MODULE = 'wasm/src/storage/protocol.ts'
COPYRIGHT = '/*\n * Copyright 2026 Shujie Zhang\n * SPDX-License-Identifier: Apache-2.0\n */\n\n'


def render(contract):
    operations, errors = contract['operations'], contract['errno']
    if contract['schema_version'] != 1 or sorted(operations.values()) != list(range(len(operations))):
        raise WasmError('invalid storage operation numbering')
    if not all(re.fullmatch('[a-z][a-zA-Z]*', name) for name in operations):
        raise WasmError('invalid storage operation name')
    if not all(re.fullmatch('E[A-Z]+', name) and type(value) is int and value > 0 for name, value in errors.items()):
        raise WasmError('invalid target errno contract')
    if not set(contract['dom_errors'].values()).issubset(errors):
        raise WasmError('unknown DOM error mapping')
    note = '// Generated from ' + MANIFEST + '; do not edit.\n'
    header = COPYRIGHT + note + '#ifndef POSTGAMMA_WASM_STORAGE_PROTOCOL_H\n#define POSTGAMMA_WASM_STORAGE_PROTOCOL_H\n\n#include <errno.h>\n\ntypedef enum PostgammaStorageOperation {\n'
    for name, value in operations.items():
        macro = re.sub(r'([a-z])([A-Z])', r'\1_\2', name).upper()
        header += f'    PGW_STORAGE_{macro} = {value},\n'
    header += '} PostgammaStorageOperation;\n\n#ifdef __EMSCRIPTEN__\n'
    for name, value in errors.items():
        expression = f'{name} == {value}, "storage errno differs from target libc"'
        header += f'#ifdef __cplusplus\nstatic_assert({expression});\n#else\n_Static_assert({expression});\n#endif\n'
    header += '#endif\n\n#endif\n'
    module = COPYRIGHT + note
    module += 'export const StorageOp = ' + json.dumps(operations, indent=2) + ' as const;\n'
    module += 'export type StorageOperation = (typeof StorageOp)[keyof typeof StorageOp];\n'
    module += 'export const StorageErrno = ' + json.dumps(errors, indent=2) + ' as const;\n'
    module += 'const domErrors: Readonly<Record<string, number>> = {\n'
    for name, code in contract['dom_errors'].items():
        module += f'  {name}: StorageErrno.{code},\n'
    module += '};\nexport function storageErrorCode(cause: unknown): number {\n'
    module += "  if (cause && typeof cause === 'object' && 'name' in cause && typeof cause.name === 'string')\n"
    module += '    return Object.hasOwn(domErrors, cause.name) ? domErrors[cause.name] : StorageErrno.EIO;\n  return StorageErrno.EIO;\n}\n'
    return {HEADER: header, MODULE: module}


def check(root, write=False):
    for name, expected in render(read_json(root / MANIFEST)).items():
        path = root / name
        if write:
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(expected)
        elif not path.is_file() or path.read_text() != expected:
            raise WasmError('stale storage contract: ' + name + '; run python3 buildsys/wasm_storage_protocol.py --root . --write')
    return {'manifest_sha256': digest(root / MANIFEST), 'outputs': {name: digest(root / name) for name in (HEADER, MODULE)}}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, required=True)
    parser.add_argument('--write', action='store_true')
    args = parser.parse_args()
    try: print(json.dumps(check(args.root, args.write), sort_keys=True))
    except (WasmError, OSError, ValueError) as error: parser.exit(2, str(error) + '\n')
