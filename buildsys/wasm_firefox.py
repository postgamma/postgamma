#!/usr/bin/env python3
"""Install unmodified Mozilla Firefox for native resource qualification."""
import argparse
import configparser
import hashlib
from pathlib import Path
import platform
import shutil
import tarfile
import tempfile
import urllib.request

from wasm_toolchain import WasmError, digest, read_json, write_json

# Playwright's Firefox registers Juggler frame/Worker agents even without a
# --juggler-pipe connection. Their undelivered messages pollute memory tests.
PIN = {
    'version': '155.0',
    'sourceStamp': '21a0961191033207dc167b842f6c251f337b0e54',
    'buildId': '20260826195058',
    'url': 'https://archive.mozilla.org/pub/firefox/releases/155.0/'
           'linux-x86_64/en-US/firefox-155.0.tar.xz',
    'checksums': 'https://archive.mozilla.org/pub/firefox/releases/155.0/SHA512SUMS',
    'sha512': '796896d2c8a4ae1260378e8a48c526a4e0f88f32599bcc83af1147a974d26e5'
              'cbc9eb0d9bfe392fffd9b14d54007f51f1ec514b5d9db396975babb38e5e07022',
    'bytes': 86978904,
}
POLICY_NAME = 'firefox/distribution/policies.json'
POLICY = {'policies': {'DisableAppUpdate': True}}


def sha512(path: Path) -> str:
    result = hashlib.sha512()
    with path.open('rb') as source:
        for block in iter(lambda: source.read(1024 * 1024), b''):
            result.update(block)
    return result.hexdigest()


def verify_bundle(bundle: Path):
    browser = bundle / 'firefox'
    required = ('firefox', 'libxul.so', 'application.ini', 'omni.ja')
    if any(not (browser / name).is_file() for name in required):
        raise WasmError('Native Firefox bundle is incomplete')
    app = configparser.RawConfigParser()
    app.read(browser / 'application.ini')
    if any(app.get('App', key, fallback='') != PIN[field]
           for key, field in (('Version', 'version'), ('SourceStamp', 'sourceStamp'), ('BuildID', 'buildId'))):
        raise WasmError('Native Firefox bundle version or source identity mismatch')
    # Mozilla's optimized omni.ja puts the ZIP directory before file data, which
    # Python's ZipFile cannot read. Member names remain uncompressed in both
    # layouts; the archive SHA512 and installed file hashes verify all bytes.
    for omni in browser.rglob('omni.ja'):
        if b'chrome/juggler/' in omni.read_bytes().lower():
            raise WasmError('Native Firefox must not contain Playwright Juggler agents')


def configure_bundle(bundle: Path, files, expected=None):
    browser = bundle / 'firefox'
    if any((browser / name).exists() for name in ('updated', 'active-update.xml', 'updates/0/update.mar')):
        raise WasmError('Pinned Firefox has staged automatic updates; archive that state before its next launch')
    policy = bundle / POLICY_NAME
    if POLICY_NAME in files or policy.is_symlink() or policy.parent.is_symlink():
        raise WasmError('Refuse to replace an upstream or linked Firefox policy')
    if expected is not None:
        if (expected.get('path') != POLICY_NAME or expected.get('value') != POLICY or
                not policy.is_file() or digest(policy) != expected.get('sha256')):
            raise WasmError('Pinned Firefox update policy changed: ' + str(policy))
    else:
        if policy.exists() and read_json(policy) != POLICY:
            raise WasmError('Pinned Firefox has an incompatible update policy: ' + str(policy))
        policy.parent.mkdir(parents=True, exist_ok=True)
        write_json(policy, POLICY)
    return {'path': POLICY_NAME, 'value': POLICY, 'sha256': digest(policy)}


def install(deps: Path) -> tuple[Path, Path]:
    if (platform.system(), platform.machine()) != ('Linux', 'x86_64'):
        raise WasmError('The pinned native Firefox bundle requires Linux x86_64')
    directory = deps / 'firefox-native' / PIN['version']
    bundle, receipt = directory / 'bundle', directory / 'receipt.json'
    if receipt.is_file():
        state = read_json(receipt)
        required = {'firefox/firefox', 'firefox/libxul.so', 'firefox/application.ini', 'firefox/omni.ja'}
        if state.get('pin') != PIN or not required <= set(state.get('files', {})):
            raise WasmError('Native Firefox cache does not match the pinned build: ' + str(receipt))
        for name, expected in state['files'].items():
            path = bundle / name
            if not path.is_file() or digest(path) != expected:
                raise WasmError('Native Firefox cache changed: ' + str(path))
        verify_bundle(bundle)
        if 'configuration' in state and not isinstance(state['configuration'], dict):
            raise WasmError('Pinned Firefox policy receipt is invalid: ' + str(receipt))
        policy = configure_bundle(bundle, state['files'], state.get('configuration'))
        if 'configuration' not in state:
            state['configuration'] = policy
            write_json(receipt, state)
        return bundle / 'firefox/firefox', receipt
    directory.mkdir(parents=True, exist_ok=True)
    archive = directory / 'bundle.tar.xz'
    if not archive.exists():
        partial = archive.with_suffix('.partial')
        try:
            print('wasm Firefox: downloading Mozilla ' + PIN['version'], flush=True)
            with urllib.request.urlopen(PIN['url'], timeout=60) as source, partial.open('wb') as target:
                shutil.copyfileobj(source, target)
            if partial.stat().st_size != PIN['bytes'] or sha512(partial) != PIN['sha512']:
                raise WasmError('Native Firefox download size or SHA512 mismatch')
            partial.replace(archive)
        finally:
            partial.unlink(missing_ok=True)
    if archive.stat().st_size != PIN['bytes'] or sha512(archive) != PIN['sha512']:
        raise WasmError('Native Firefox archive size or SHA512 mismatch: ' + str(archive))
    if bundle.exists():
        raise WasmError('Incomplete native Firefox cache without a receipt: ' + str(bundle))
    with tempfile.TemporaryDirectory(prefix='extract-', dir=directory) as temporary:
        staging = Path(temporary) / 'bundle'
        with tarfile.open(archive, 'r:xz') as source:
            source.extractall(staging, filter='data')
        verify_bundle(staging)
        files = {str(path.relative_to(staging)): digest(path)
                 for path in sorted(staging.rglob('*')) if path.is_file()}
        policy = configure_bundle(staging, files)
        staging.replace(bundle)
    write_json(receipt, {'kind': 'postgamma.wasm-native-firefox', 'pin': PIN,
                        'files': files, 'configuration': policy,
                        'executable': str(bundle / 'firefox/firefox'),
                        'scope': 'Unmodified Mozilla browser for native memory tests; '
                                 'no Playwright Juggler frame/Worker agents; '
                                 'automatic browser updates disabled by a separately recorded policy.'})
    return bundle / 'firefox/firefox', receipt


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, required=True)
    parser.add_argument('--deps-dir', type=Path)
    args = parser.parse_args()
    try:
        executable, receipt = install((args.deps_dir or args.root / '.deps/wasm').resolve())
        print('wasm Firefox: ' + str(executable) + '; ' + str(receipt))
    except Exception as error:
        parser.exit(2, 'wasm Firefox: ' + str(error) + '\n')


if __name__ == '__main__':
    main()
