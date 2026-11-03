"""Materialize reviewed target-only PostgreSQL source adaptations."""
from __future__ import annotations
from pathlib import Path
import hashlib
import sys
from wasm_toolchain import WasmError, digest, read_json, run, write_json

HEADER = 'src/include/c.h'
MARKER = '#else\n#error "cannot identify target architecture"\n#endif'
REPLACEMENT = '#elif defined(__wasm__)\n/* The target compiler already supplies the WebAssembly architecture macro. */\n' + MARKER


GUC_SOURCE = 'src/backend/utils/misc/guc_tables.c'
GUC_MARKER = '#ifndef EXEC_BACKEND\n\t{"mmap", SHMEM_TYPE_MMAP, false},\n#endif'
GUC_REPLACEMENT = '#if !defined(EXEC_BACKEND) || defined(__wasm__)\n\t{"mmap", SHMEM_TYPE_MMAP, false},\n#endif'
SHMEM_HEADER = 'src/include/storage/pg_shmem.h'
SHMEM_MARKER = '#if !defined(WIN32) && !defined(EXEC_BACKEND)\n#define DEFAULT_SHARED_MEMORY_TYPE SHMEM_TYPE_MMAP'
SHMEM_REPLACEMENT = '#if !defined(WIN32) && (!defined(EXEC_BACKEND) || defined(__wasm__))\n#define DEFAULT_SHARED_MEMORY_TYPE SHMEM_TYPE_MMAP'
FD_SOURCE = 'src/backend/storage/file/fd.c'
FLUSH_MARKER = '''/* Define PG_FLUSH_DATA_WORKS if we have an implementation for pg_flush_data */
#if defined(HAVE_SYNC_FILE_RANGE)
#define PG_FLUSH_DATA_WORKS 1
#elif !defined(WIN32) && defined(MS_ASYNC)
#define PG_FLUSH_DATA_WORKS 1
#elif defined(USE_POSIX_FADVISE) && defined(POSIX_FADV_DONTNEED)
#define PG_FLUSH_DATA_WORKS 1
#endif'''
FLUSH_REPLACEMENT = '''/* WasmFS has no OS page cache to preflush. Its mmap/msync fallback copies
 * file data through linear memory, so recovery must use the real fsync pass
 * without first traversing and mapping every file for an advisory hint. */
#if !defined(__EMSCRIPTEN__)
''' + FLUSH_MARKER + '\n#endif'
RULES = {
    HEADER: (MARKER, REPLACEMENT, 'accept the compiler WebAssembly architecture without impersonating a host'),
    GUC_SOURCE: (GUC_MARKER, GUC_REPLACEMENT, 'expose the threaded Wasm shared-memory provider in the GUC enum'),
    SHMEM_HEADER: (SHMEM_MARKER, SHMEM_REPLACEMENT, 'select the Wasm shared-memory provider as its configuration default'),
    FD_SOURCE: (FLUSH_MARKER, FLUSH_REPLACEMENT, 'skip recovery preflush on WasmFS while preserving the durable fsync traversal'),
}


def adaptations(source: Path) -> list[dict]:
    changes = []
    for name, (marker, replacement, reason) in RULES.items():
        before = (source / name).read_bytes()
        if replacement.encode() in before:
            continue
        if name == HEADER and (b'cannot identify target architecture' not in before or b'defined(__wasm__)' in before):
            continue
        if before.count(marker.encode()) != 1:
            raise WasmError('unreviewed PostgreSQL target source shape: ' + name)
        after = before.replace(marker.encode(), replacement.encode())
        changes.append({'path': name, 'reason': reason,
                        'before': hashlib.sha256(before).hexdigest(), 'after': hashlib.sha256(after).hexdigest()})
    return changes


def apply_adaptations(tree: Path, changes: list[dict]) -> None:
    for change in changes:
        path = tree / change['path']
        if digest(path) != change['before']:
            raise WasmError('target source adaptation input changed: ' + change['path'])
        marker, replacement, _ = RULES[change['path']]
        text = path.read_bytes().replace(marker.encode(), replacement.encode())
        if hashlib.sha256(text).hexdigest() != change['after']:
            raise WasmError('target source adaptation output mismatch')
        path.write_bytes(text)


def source_tree(config, directory: Path, inputs: dict) -> Path:
    changes = inputs['source_adaptations']
    if not changes:
        return config.root / 'postgres'
    tree = directory / 'source/postgres'
    stamp = directory / 'source/receipt.json'
    def hashes():
        return {str(p.relative_to(tree)): digest(p) for p in sorted(tree.rglob('*')) if p.is_file()}
    if stamp.is_file():
        receipt = read_json(stamp)
        if receipt['commit'] != inputs['postgres_commit'] or receipt['adaptations'] != changes or receipt['files'] != hashes():
            raise WasmError('adapted PostgreSQL capture source changed')
        return tree
    run([sys.executable, str(config.root / 'buildsys/materialize.py'),
         '--repository', str(config.root / 'postgres'), '--manifest', str(config.root / 'manifests/upstream.json'),
         '--destination', str(tree), '--build-root', str(config.build)],
        directory / 'source/materialize.log', cwd=config.root)
    apply_adaptations(tree, changes)
    write_json(stamp, {'commit': inputs['postgres_commit'], 'adaptations': changes, 'files': hashes()})
    return tree
