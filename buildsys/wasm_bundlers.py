#!/usr/bin/env python3
"""Build the installed browser package with pinned Vite and webpack fixtures."""
from __future__ import annotations
import argparse
from pathlib import Path
import shutil
from wasm import serve
from wasm_matrix import latest_package
from wasm_package import verify_tree
from wasm_toolchain import (WasmError, browser_environment, configuration, digest, read_json,
                            run, run_id, sdk_tools, write_json)


def check(config, package_report=None):
    package_report = package_report or latest_package(config)
    package = read_json(package_report)
    archive = Path(package['archive']['path'])
    if package['status'] != 'passed' or digest(archive) != package['archive']['sha256']:
        raise WasmError('Bundler checks require an unchanged production package')
    if verify_tree(Path(package['package']), str(config.root)) != package['staged']:
        raise WasmError('Production package tree changed')
    for name, expected in package['sources'].items():
        if name.startswith(('wasm/src/', 'wasm/tooling/')) and digest(config.root / name) != expected:
            raise WasmError('Browser SDK changed; rebuild the package')
    current = config.build / 'wasm/bundlers' / run_id(); current.mkdir(parents=True)
    fixture = config.root / 'tests/wasm/package/bundlers'
    sources = [*fixture.glob('*'), *sorted((config.root / 'examples/wasm/bundlers').glob('*')),
               config.root / 'tests/wasm/package/bundlers.mjs', config.root / 'buildsys/wasm_bundlers.py']
    report = {'kind': 'postgamma.wasm-bundlers', 'status': 'failed', 'package_report': str(package_report),
              'archive': package['archive'], 'cases': {},
              'sources': {str(p.relative_to(config.root)): digest(p) for p in sources if p.is_file()}}
    tools, env = sdk_tools(config)
    try:
        dependencies = config.deps / 'bundlers' / digest(fixture / 'package-lock.json')[:16]
        dependencies.mkdir(parents=True, exist_ok=True)
        for name in ('package.json', 'package-lock.json'): shutil.copy2(fixture / name, dependencies / name)
        if not (dependencies / '.installed').is_file():
            run([tools['npm'], 'ci', '--ignore-scripts', '--no-audit', '--no-fund'],
                current / 'npm-ci.log', env, dependencies, timeout=300)
            (dependencies / '.installed').write_text(digest(fixture / 'package-lock.json') + '\n')
        for name, version in read_json(fixture / 'package.json')['devDependencies'].items():
            if read_json(dependencies / 'node_modules' / name / 'package.json')['version'] != version:
                raise WasmError('Installed bundler version differs from the lockfile: ' + name)
        application = current / 'app'; application.mkdir()
        write_json(application / 'package.json', {'name': 'postgamma-bundler-consumer', 'private': True, 'type': 'module'})
        run([tools['npm'], 'install', '--ignore-scripts', '--no-audit', '--no-fund', '--offline', str(archive)],
            current / 'package-install.log', env, application)
        for source in (config.root / 'examples/wasm/bundlers').glob('*'):
            if source.is_file(): shutil.copy2(source, application / source.name)
        commands = {
            'vite': [tools['node'], str(dependencies / 'node_modules/vite/bin/vite.js'), 'build'],
            'webpack': [tools['node'], str(dependencies / 'node_modules/webpack-cli/bin/cli.js'), '--config', 'webpack.config.mjs'],
        }
        report['versions'] = read_json(fixture / 'package.json')['devDependencies']
        for name, command in commands.items():
            print('wasm bundler: ' + name, flush=True)
            run(command, current / (name + '-build.log'), env, application, timeout=120)
            with serve(application / 'dist', 'index.html') as url:
                browser = [tools['node'], str(config.root / 'tests/wasm/package/bundlers.mjs'),
                    '--root', str(config.root), '--url', url, '--output', str(current / (name + '-browser.json'))]
                if config.browser_override: browser += ['--executable', config.browser_override]
                run(browser, current / (name + '-browser.log'), browser_environment(config, env), config.root, timeout=150)
            report['cases'][name] = read_json(current / (name + '-browser.json'))
            write_json(current / 'report.json', report)
        if any(digest(config.root / name) != expected for name, expected in report['sources'].items()):
            raise WasmError('Bundler fixture changed during validation')
        report['status'] = 'passed'
    except Exception as error:
        report['error'] = str(error); raise
    finally:
        write_json(current / 'report.json', report)
        write_json(config.build / 'wasm/reports/bundlers.json', {'status': report['status'], 'run_report': str(current / 'report.json')})
        print(f"wasm bundlers: {report['status']}; {current / 'report.json'}", flush=True)
    return current / 'report.json'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, required=True)
    for name in ('build-dir', 'deps-dir', 'sdk-dir', 'profile'): parser.add_argument('--' + name, type=Path)
    parser.add_argument('--target', default='wasm32-emscripten-pthreads')
    parser.add_argument('--mode', default='release', choices=('debug', 'release'))
    parser.add_argument('--node', default=''); parser.add_argument('--browser-executable', default='')
    parser.add_argument('--longjmp', default='emscripten'); parser.add_argument('--jobs', type=int, default=1)
    parser.add_argument('--package-report', type=Path)
    args = parser.parse_args()
    try: check(configuration(args), args.package_report)
    except Exception as error: parser.exit(2, 'wasm bundlers: ' + str(error).splitlines()[0] + '\n')


if __name__ == '__main__': main()
