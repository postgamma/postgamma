#!/usr/bin/env python3
"""Install the upstream WebKit Worker ownership repair for native memory tests."""
import argparse
from pathlib import Path
import platform
import shutil
import tarfile
import tempfile
import urllib.request

from wasm_toolchain import WasmError, digest, read_json, write_json

# The official bundle includes 322643@main (JSVMClientData owns JSHeapData).
# It is separate from Playwright: its automation protocol is not compatible.
PIN = {
    'revision': '322662@main',
    'commit': '9eb4e2ad5983af0d494b62b38e55ff9010fb5993',
    'ownershipFix': '6cf979a79f7bb78c0635ff43f02282d78137583e',
    'url': 'https://wpewebkit.org/built-products/x86_64/release/nightly/MiniBrowser/'
           'MiniBrowser_wpe_322662@main.tar.xz',
    'sha256': '3fdcfd73e1f634b7d9df53de95dcaecc319dfd102fb0af7faec3a6d8e491e411',
    'bytes': 240315992,
}


def install(deps: Path) -> tuple[Path, Path]:
    if (platform.system(), platform.machine()) != ('Linux', 'x86_64'):
        raise WasmError('The pinned native WPE bundle requires Linux x86_64')
    directory = deps / 'webkit-native' / PIN['revision']
    bundle, receipt = directory / 'bundle', directory / 'receipt.json'
    if receipt.is_file():
        state = read_json(receipt)
        if state.get('pin') != PIN or not state.get('files'):
            raise WasmError('Native WebKit cache does not match the pinned build: ' + str(receipt))
        for name, expected in state['files'].items():
            path = bundle / name
            if not path.is_file() or digest(path) != expected:
                raise WasmError('Native WebKit cache changed: ' + str(path))
        return bundle / 'MiniBrowser', receipt
    directory.mkdir(parents=True, exist_ok=True)
    archive = directory / 'bundle.tar.xz'
    if not archive.exists():
        partial = archive.with_suffix('.partial')
        try:
            print('wasm WebKit: downloading upstream ' + PIN['revision'], flush=True)
            with urllib.request.urlopen(PIN['url'], timeout=60) as source, partial.open('wb') as target:
                shutil.copyfileobj(source, target)
            if partial.stat().st_size != PIN['bytes'] or digest(partial) != PIN['sha256']:
                raise WasmError('Native WebKit download size or SHA256 mismatch')
            partial.replace(archive)
        finally:
            partial.unlink(missing_ok=True)
    if archive.stat().st_size != PIN['bytes'] or digest(archive) != PIN['sha256']:
        raise WasmError('Native WebKit archive size or SHA256 mismatch: ' + str(archive))
    if bundle.exists():
        raise WasmError('Incomplete native WebKit cache without a receipt: ' + str(bundle))
    with tempfile.TemporaryDirectory(prefix='extract-', dir=directory) as temporary:
        staging = Path(temporary) / 'bundle'
        with tarfile.open(archive, 'r:xz') as source:
            source.extractall(staging, filter='data')
        if ('WebKit Revision: ' + PIN['revision']) not in (staging / 'README.txt').read_text():
            raise WasmError('Native WebKit bundle revision mismatch')
        if not (staging / 'MiniBrowser').is_file():
            raise WasmError('Native WebKit bundle has no MiniBrowser wrapper')
        files = {str(path.relative_to(staging)): digest(path)
                 for path in sorted(staging.rglob('*')) if path.is_file()}
        staging.replace(bundle)
    write_json(receipt, {'kind': 'postgamma.wasm-native-webkit', 'pin': PIN,
                        'files': files, 'executable': str(bundle / 'MiniBrowser'),
                        'scope': 'Unmodified upstream browser for native memory tests; '
                                 'not a Playwright executable or a PostgreSQL/SDK patch.'})
    return bundle / 'MiniBrowser', receipt


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, required=True)
    parser.add_argument('--deps-dir', type=Path)
    args = parser.parse_args()
    try:
        executable, receipt = install((args.deps_dir or args.root / '.deps/wasm').resolve())
        print('wasm WebKit: ' + str(executable) + '; ' + str(receipt))
    except Exception as error:
        parser.exit(2, 'wasm WebKit: ' + str(error) + '\n')


if __name__ == '__main__': main()
