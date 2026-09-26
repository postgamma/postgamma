#!/usr/bin/env python3
"""Select native and platform regression from changed kernel and SDK inputs."""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess


def shared_input(name):
    if name in ('Makefile', 'VERSION', 'postgres', 'third_party/pgvector',
                '.github/workflows/wasm.yml'):
        return True
    if name.startswith(('runtime/', 'embedded-c/', 'compiler/', 'extensions/pgvector/', 'tests/embedded/', 'tests/runtime/')):
        return True
    if name.startswith('manifests/'):
        return not name.startswith('manifests/profiles/wasm-')
    if name.startswith('buildsys/'):
        return not name.rsplit('/', 1)[1].startswith(('wasm', 'check_wasm', 'plan_wasm', 'collect_wasm'))
    return False


def platform_input(name):
    if shared_input(name):
        return True
    return name.startswith(('wasm/bridge/', 'wasm/package', 'manifests/profiles/wasm-',
        'tests/wasm/probes/', 'tests/wasm/runtime/', 'tests/buildsys/test_wasm',
        'buildsys/wasm', 'buildsys/check_wasm', 'buildsys/plan_wasm',
        '.github/actions/wasm-setup/', '.github/workflows/wasm.yml',
        '.github/workflows/upstream-canary.yml'))


def plan(root, base):
    if not base or set(base) == {'0'}:
        return {'native': True, 'platform': True, 'reason': 'manual run or new branch', 'paths': []}
    if not re.fullmatch('[0-9a-fA-F]{40}|[0-9a-fA-F]{64}', base):
        raise ValueError('base must be a commit ID')
    try:
        paths = subprocess.check_output(['git', '-C', str(root), 'diff', '--name-only', '-z', base, 'HEAD', '--'], timeout=30).decode().split('\0')
    except subprocess.SubprocessError:
        return {'native': True, 'platform': True, 'reason': 'base history unavailable', 'paths': []}
    paths = [path for path in paths if path]
    return {'native': any(shared_input(path) for path in paths),
            'platform': any(platform_input(path) for path in paths),
            'reason': 'changed source paths', 'paths': paths}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, required=True)
    parser.add_argument('--base', default='')
    args = parser.parse_args()
    try: result = plan(args.root, args.base)
    except ValueError as error: parser.error(str(error))
    if output := os.environ.get('GITHUB_OUTPUT'):
        with Path(output).open('a') as stream:
            for name in ('native', 'platform'):
                stream.write(name + '=' + str(result[name]).lower() + '\n')
    print(json.dumps(result, sort_keys=True))

if __name__ == '__main__': main()
