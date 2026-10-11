#!/usr/bin/env python3
"""Collect bounded CI diagnostics without browser profiles or generated trees."""
import argparse
from pathlib import Path
import shutil
from wasm_toolchain import digest, write_json


def selected(path):
    parts = path.parts
    if 'upstream' in parts and 'project' in parts:
        offset = parts.index('project') + 1
        if parts[offset:offset + 2] != ('build', 'wasm'):
            return False
    if any(part in ('node_modules', 'objects', 'profile', 'package', 'site', 'generated', 'postgres-build', 'capture-build') or
           part.startswith(('consumer-', 'browser-profile')) for part in parts):
        return False
    if path.suffix not in ('.json', '.jsonl', '.log'):
        return False
    return bool(set(parts) & {'runs', 'reports', 'ci', 'matrix', 'lifecycle', 'multitab', 'pglite', 'archive', 'logical', 'contrib', 'parallel', 'sdk', 'bundlers', 'soak', 'doctor', 'upstream'}) or path.name == 'inputs.json'


def collect(build, output):
    build, output = build.resolve(), output.resolve()
    if output == build or not output.is_relative_to(build):
        raise ValueError('evidence destination must be below the build directory')
    files, omitted = {}, []
    for source in sorted((build/'wasm').rglob('*')):
        if source.is_symlink() or not source.is_file() or source.is_relative_to(output): continue
        relative = source.relative_to(build)
        if not selected(relative) or not source.resolve().is_relative_to(build): continue
        if source.stat().st_size > 16 * 1024 * 1024:
            omitted.append({'path':str(relative), 'bytes':source.stat().st_size, 'reason':'file size limit'})
            continue
        target = output/relative; target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, target)
        files[str(relative)] = {'sha256':digest(target), 'bytes':target.stat().st_size}
    write_json(output/'collection.json', {'kind':'postgamma.wasm-ci-evidence', 'files':files,
               'bytes':sum(value['bytes'] for value in files.values()), 'omitted':omitted})
    print(f'wasm evidence: {len(files)} files; {output}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    collect(args.build_dir, args.output)

if __name__ == '__main__': main()
