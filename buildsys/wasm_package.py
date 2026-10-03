#!/usr/bin/env python3
"""Build and validate a relocatable browser consumer package without publishing."""
from __future__ import annotations
import argparse
import json
from pathlib import Path
import shutil
import tempfile

from wasm import serve
from wasm_assets import npm_version
from wasm_sql import startup
from wasm_contrib import CONTRIB, selection
from wasm_toolchain import (WasmError, browser_environment, capture, configuration, digest, fingerprint,
                            read_json, run, sdk_tools, write_json)


def verify_tree(package: Path, forbidden: str = '') -> dict:
    metadata = read_json(package / 'package.json')
    manifest = read_json(package / 'postgamma.assets.json')
    if metadata.get('dependencies') or metadata.get('scripts') or metadata.get('devDependencies'):
        raise WasmError('consumer package must not install tools or runtime dependencies')
    if metadata['version'] != manifest['version'] or manifest['bridge']['testing']:
        raise WasmError('consumer package version or production bridge mismatch')
    for name, expected in manifest['files'].items():
        path = package / name
        if not path.resolve().is_relative_to(package.resolve()) or path.is_symlink():
            raise WasmError('package asset is outside its directory')
        if not path.is_file() or digest(path) != expected['sha256'] or path.stat().st_size != expected['bytes']:
            raise WasmError('package asset mismatch: ' + name)
    files = sorted(p for p in package.rglob('*') if p.is_file())
    if any(forbidden and forbidden.encode() in p.read_bytes() for p in files):
        raise WasmError('consumer package contains a checkout path')
    for required in ('src/index.js', 'src/index.d.ts', 'LICENSE', 'NOTICE', 'licenses/LICENSE.postgresql',
                     'licenses/LICENSE.emscripten', 'licenses/LICENSE.musl'):
        if not (package / required).is_file():
            raise WasmError('consumer package is missing ' + required)
    return {'files': {str(p.relative_to(package)): digest(p) for p in files},
            'bytes': sum(p.stat().st_size for p in files), 'build_id': manifest['buildId'],
            'version': metadata['version']}


def consumer(config, tools, env, archive: Path, current: Path, engine='chromium', faults=True, storage='opfs') -> dict:
    # A fresh install owns its files. No symlink to the staging tree or source is served.
    directory = current / ('consumer-' + engine)
    directory.mkdir(parents=True)
    write_json(directory / 'package.json', {'name': 'postgamma-independent-consumer', 'private': True, 'type': 'module'})
    run([tools['npm'], 'install', '--ignore-scripts', '--no-audit', '--no-fund', '--offline', str(archive)],
        current / ('install-' + engine + '.log'), env, directory)
    package = directory / 'node_modules/@postgamma/wasm'
    evidence = verify_tree(package, str(config.root))
    for name in ('consumer.html', 'consumer.js', 'consumer.ts'):
        shutil.copy2(config.root / 'tests/wasm/package' / name, directory / name)
    from wasm_pgvector_fixtures import fixtures
    fixture = fixtures(config.root, current, directory)
    run([tools['node'], str(config.package / 'node_modules/typescript/bin/tsc'), '--strict', '--noEmit',
         '--target', 'ES2022', '--module', 'ESNext', '--moduleResolution', 'Bundler',
         '--lib', 'ES2022,DOM', 'consumer.ts'], current / ('consumer-types-' + engine + '.log'), env, directory)
    with serve(directory, 'consumer.html') as url:
        command = [tools['node'], str(config.root / 'tests/wasm/package/run.mjs'), '--root', str(config.root),
                   '--url', url, '--output', str(current / ('browser-' + engine + '.json')), '--engine', engine, '--storage', storage]
        if not faults: command += ['--no-faults']
        launcher = config.deps / 'browser-libs/launch-webkit.sh'
        if engine == 'webkit' and launcher.is_file(): command += ['--executable', str(launcher)]
        elif engine == 'chromium' and config.browser_override: command += ['--executable', config.browser_override]
        with tempfile.TemporaryDirectory(prefix='pgm-consumer-', dir='/tmp') as temporary:
            run(command, current / ('browser-' + engine + '.log'),
                dict(browser_environment(config, env), TMPDIR=temporary), config.root, timeout=360 if storage == 'all' else 240)
    browser = read_json(current / ('browser-' + engine + '.json'))
    if verify_tree(package, str(config.root)) != evidence:
        raise WasmError('installed consumer assets changed during validation')
    return {'status': browser['status'], 'installed': evidence, 'fixture': fixture, 'browser': browser}


def package(config, longjmp, extensions, check=True):
    source_paths = [*sorted((config.package / 'src').rglob('*.ts')),
                    *sorted((config.root / 'tests/wasm/package').glob('*')),
                    *(config.package / name for name in ('package.json', 'package-lock.json', 'tsconfig.json', 'PACKAGE_README.md')),
                    *(config.root / name for name in ('VERSION', 'LICENSE', 'NOTICE'))]
    inputs = {str(path.relative_to(config.root)): digest(path) for path in source_paths if path.is_file()}
    receipt = startup(config, longjmp, 'build', 'abi', extensions)
    sql = read_json(receipt)
    site = Path(sql['site'])
    current, variant = receipt.parent, site.parent.parent
    tools, env = sdk_tools(config)
    report = {'kind': 'postgamma.wasm-package', 'status': 'failed', 'link_report': sql['link_report'], 'sources': inputs}
    try:
        stage = current / 'package'
        stage.mkdir()
        manifest = read_json(site / 'postgamma.assets.json')
        for name in (*manifest['files'], 'postgamma.assets.json'):
            target = stage / name; target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(site / name, target)
        run([tools['node'], str(config.package / 'node_modules/typescript/bin/tsc'), '-p',
             str(config.package / 'tsconfig.json'), '--outDir', str(stage / 'src')],
            current / 'package-types.log', env, config.root)
        for name in ('protocol/limits.d.ts', 'loader/identity.d.ts'):
            shutil.copy2(config.package / 'src' / name, stage / 'src' / name)
        metadata = {'name': '@postgamma/wasm', 'version': npm_version(config.root), 'type': 'module',
                    'description': 'PostgreSQL inside your browser application', 'license': 'Apache-2.0',
                    'homepage': 'https://postgamma.com',
                    'repository': {'type': 'git', 'url': 'https://github.com/postgamma/postgamma.git'},
                    'exports': {'.': {'types': './src/index.d.ts', 'import': './src/index.js'},
                                './assets/*': './*', './package.json': './package.json'},
                    'types': './src/index.d.ts', 'files': ['src/', 'postgamma.*', 'licenses/', 'LICENSE', 'NOTICE', 'README.md']}
        write_json(stage / 'package.json', metadata)
        for name in ('LICENSE', 'NOTICE'):
            shutil.copy2(config.root / name, stage / name)
        shutil.copy2(config.root / 'wasm/PACKAGE_README.md', stage / 'README.md')
        licenses = stage / 'licenses'; licenses.mkdir()
        for name in ('postgresql', *(name for name in extensions if name not in CONTRIB)):
            shutil.copy2(config.root / ('licenses/LICENSE.' + name), licenses / ('LICENSE.' + name))
        sdk = config.sdk / 'upstream/emscripten'
        for label, path in {'emscripten': 'LICENSE', 'musl': 'system/lib/libc/musl/COPYRIGHT',
                            'compiler-rt': 'system/lib/compiler-rt/LICENSE.TXT',
                            'libcxx': 'system/lib/libcxx/LICENSE.TXT',
                            'libcxxabi': 'system/lib/libcxxabi/LICENSE.TXT',
                            'libunwind': 'system/lib/libunwind/LICENSE.TXT'}.items():
            shutil.copy2(sdk / path, licenses / ('LICENSE.' + label))
        with (stage / 'NOTICE').open('a') as notice:
            notice.write('\nIncludes PostgreSQL, Emscripten, musl, LLVM runtime libraries' +
                         (' and pgvector' if extensions else '') + '.\nSee licenses/ for their notices and terms.\n')
        report['staged'] = verify_tree(stage, str(config.root))
        destination = variant / 'dist'; destination.mkdir(exist_ok=True)
        packed = json.loads(capture([tools['npm'], 'pack', '--ignore-scripts', '--json',
                                     '--pack-destination', str(destination)], env, stage))[0]
        archive = destination / packed['filename']
        report['archive'] = {'path': str(archive), 'sha256': digest(archive), 'bytes': archive.stat().st_size}
        if check:
            report['consumer'] = consumer(config, tools, env, archive, current)
        if any(digest(config.root / name) != checksum for name, checksum in inputs.items()):
            raise WasmError('package source or consumer harness changed during validation')
        report['consumer_validated'] = check
        # Retain the exact staged tree under its content identity for independent consumers.
        published = variant / 'package' / fingerprint(report['staged']['files'])
        published.parent.mkdir(exist_ok=True)
        if published.exists():
            if verify_tree(published, str(config.root)) != report['staged']:
                raise WasmError('package identity already exists with different contents')
            shutil.rmtree(stage)
        else: stage.rename(published)
        report['package'] = str(published)
        report['status'] = 'passed'
        print(f'wasm package archive: {archive}', flush=True)
    except Exception as error:
        report['error'] = str(error)
        raise
    finally:
        write_json(current / 'package.json', report)
        write_json(variant / 'reports/package.json', {'status': report['status'], 'run_report': str(current / 'package.json')})
        print(f"wasm package: {report['status']}; {current / 'package.json'}", flush=True)
    return current / 'package.json'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=('build', 'check'))
    parser.add_argument('--root', type=Path, required=True)
    for name in ('build-dir', 'deps-dir', 'sdk-dir', 'profile'): parser.add_argument('--' + name, type=Path)
    parser.add_argument('--target', default='wasm32-emscripten-pthreads')
    parser.add_argument('--mode', choices=('debug', 'release'), default='debug')
    parser.add_argument('--longjmp', default='emscripten')
    parser.add_argument('--node', default=''); parser.add_argument('--browser-executable', default='')
    parser.add_argument('--jobs', type=int, default=2)
    parser.add_argument('--extensions', default='standard')
    args = parser.parse_args()
    try: package(configuration(args), args.longjmp, selection(args.extensions), args.action == 'check')
    except Exception as error: parser.exit(2, 'wasm package: ' + str(error).splitlines()[0] + '\n')


if __name__ == '__main__': main()
