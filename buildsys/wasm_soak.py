#!/usr/bin/env python3
"""Seeded mixed-load browser endurance gate with replay and resource receipts."""
import argparse
from pathlib import Path
import platform
import shutil
import tempfile
import time
from urllib.parse import urlencode

from wasm import serve
from wasm_contrib import STANDARD
from wasm_features import verify
from wasm_link import source_snapshot
from wasm_sql import startup
from wasm_toolchain import (WasmError, browser_environment, configuration, digest,
                            read_json, run, run_id, sdk_tools, write_json)
from wasm_webkit import install as install_webkit

ENGINES = ('chromium', 'firefox', 'webkit')
CHECKS = {'soak-four-connections', 'soak-commit-model', 'soak-ddl-savepoint-rollback',
          'soak-copy', 'soak-notification-barrier', 'soak-sql-error-reuse',
          'soak-clean-reopen', 'soak-crash-recovery', 'soak-cancel-reuse'}


def validate(row, parameters):
    if row.get('kind') == 'postgamma.wasm-memory-diagnostic':
        raise WasmError('Memory profiling cannot qualify a production resource gate')
    if row.get('status') != 'passed' or not CHECKS <= set(row.get('probe', {}).get('checks', [])):
        raise WasmError('Mixed workload did not complete all checks')
    actual, complete = row.get('parameters', {}), row.get('complete', {})
    for name in ('seconds', 'seed', 'rounds', 'reopenEvery'):
        if actual.get(name) != parameters[name]:
            raise WasmError('Mixed workload parameter mismatch: ' + name)
    if parameters.get('faultSource', 'native-pthread') != actual.get('faultSource', 'native-pthread'):
        raise WasmError('Mixed workload failure source mismatch')
    if parameters['rounds']:
        if complete.get('iteration') != parameters['rounds']:
            raise WasmError('Replay did not complete the requested rounds')
    elif complete.get('elapsedMs', 0) < parameters['seconds'] * 1000:
        raise WasmError('Mixed workload ended before the requested duration')
    if any(complete.get(name, 0) < 1 for name in ('iteration', 'crashes', 'orderly', 'cancellations')):
        raise WasmError('Missing mixed workload or recovery evidence')
    if not parameters['rounds'] and parameters['seconds'] >= 300 and row.get('resourceGate', {}).get('status') != 'passed':
        raise WasmError('Long-run resource growth gate did not pass')
    if row.get('driver') in ('native-wpe', 'native-firefox'):
        owner = row.get('workerOwnership', {})
        expected = 'webkit' if row['driver'] == 'native-wpe' else 'firefox'
        if (row.get('engine') != expected or row.get('finalPhysicalWorkerThreads') != 0 or
                owner.get('active') != [] or owner.get('activePthreads') != [] or
                owner.get('nativeOwnerPosts') != 0 or
                not owner.get('created') or owner.get('created') != owner.get('terminated') or
                not owner.get('pthreadCreated') or owner.get('pthreadCreated') != owner.get('pthreadTerminated')):
            raise WasmError('Native browser Worker ownership or physical retirement evidence is incomplete')
        idle = row.get('finalIdle', {})
        if (idle.get('seconds') != 60 or idle.get('elapsedSeconds', 0) < 60 or
                idle.get('physicalWorkerThreads') != 0):
            raise WasmError('Native browser final idle observation did not complete')
        if row.get('browserProtocolCommands') != []:
            raise WasmError('Native browser acceptance must not attach an automation protocol')


def production_package(config, receipt):
    from wasm_package import verify_tree
    package = read_json(receipt)
    if package.get('status') != 'passed' or not package.get('consumer_validated'):
        raise WasmError('Production endurance requires an independently installed package')
    directory = Path(package['package'])
    if verify_tree(directory, str(config.root)) != package['staged']:
        raise WasmError('Production package changed before endurance')
    if digest(Path(package['archive']['path'])) != package['archive']['sha256']:
        raise WasmError('Production npm archive changed before endurance')
    manifest = read_json(directory / 'postgamma.assets.json')
    if (manifest['mode'] != config.mode or manifest['bridge']['testing'] or
            set(manifest['extensions']) != set(STANDARD)):
        raise WasmError('Endurance package must use the requested mode, standard extensions and production bridge')
    for name, expected in package['sources'].items():
        if digest(config.root / name) != expected:
            raise WasmError('Production package source changed: ' + name)
    return package


def check(config, longjmp, engines, parameters, growth_mib, package_receipt=None):
    if not engines or len(set(engines)) != len(engines) or any(e not in ENGINES for e in engines):
        raise WasmError('Invalid soak browser selection')
    if not 1 <= parameters['seconds'] <= 28800 or not 1 <= parameters['seed'] <= 0xffffffff:
        raise WasmError('Duration or uint32 seed is out of range')
    if not 0 <= parameters['rounds'] <= 1000000 or parameters['reopenEvery'] < 2 or growth_mib < 0:
        raise WasmError('Invalid replay, reopen or memory budget')
    current = config.build / 'wasm/soak' / run_id(); current.mkdir(parents=True)
    report = {'kind': 'postgamma.wasm-soak', 'status': 'failed', 'mode': config.mode,
              'parameters': parameters, 'requested_engines': engines, 'engines': {}, 'growthMiB': growth_mib}
    runner_sources = {name: digest(config.root / name) for name in
        ('buildsys/wasm_soak.py', 'buildsys/wasm_webkit.py', 'tests/wasm/soak/browser.mjs',
         'tests/wasm/soak/native-webkit.mjs',
         'tests/wasm/soak/native.mjs', 'tests/wasm/soak/native-relay.js',
         'tests/wasm/soak/observer.mjs',
         'tests/wasm/soak/memory.mjs', 'tests/wasm/soak/processes.mjs', 'tests/wasm/soak/resources.mjs',
         'tests/wasm/soak/resources.test.mjs', 'tests/wasm/soak/processes.test.mjs',
         'tests/wasm/browser/diagnostics.mjs')}
    report['runner_sources'] = runner_sources
    started = time.monotonic()
    try:
        native_webkit = None
        if 'webkit' in engines and platform.system() == 'Linux':
            native_webkit, native_receipt = install_webkit(config.deps)
            report['nativeBrowserReceipt'] = {'path': str(native_receipt), 'sha256': digest(native_receipt)}
        # Other gates may relink the shared variant while this long run is active.
        # Freeze all loaded assets so reopen always uses the same exact build.
        site = current / 'site'
        tools, env = sdk_tools(config)
        if package_receipt:
            package = production_package(config, package_receipt)
            report['productionPackage'] = {'receipt': str(package_receipt), 'archive': package['archive'],
                'buildId': package['staged']['build_id'], 'bridgeTesting': False,
                'scope': 'Exact production npm assets; control Worker errors/rejections. Native pthread trap/abort coverage belongs to the separate lifecycle matrix.'}
            shutil.copytree(Path(package['package']), site)
            sources = {}
            # Add workload files beside the installed package. Never replace
            # or recompile an SDK/kernel file or its production manifest.
            for folder, target in (('sql', 'tests'), ('storage', 'storage-tests'), ('vector', 'vector-tests')):
                for path in sorted((config.root / 'tests/wasm' / folder).glob('*.ts')):
                    sources[str(path.relative_to(config.root))] = digest(path)
                    run([tools['node'], str(config.root / 'buildsys/wasm_typescript.mjs'), str(path),
                         str(site / target / path.with_suffix('.js').name)], current / 'typescript.log', env, config.root)
            html = config.root / 'tests/wasm/sql/sql.html'
            (site / 'sql.html').write_bytes(html.read_bytes()); sources[str(html.relative_to(config.root))] = digest(html)
            example = config.root / 'examples/wasm/vector-search.ts'
            sources[str(example.relative_to(config.root))] = digest(example)
            run([tools['node'], str(config.root / 'buildsys/wasm_typescript.mjs'), str(example),
                 str(site / 'vector-example.js')], current / 'typescript.log', env, config.root)
            for name, expected in package['staged']['files'].items():
                if digest(site / name) != expected:
                    raise WasmError('Workload preparation changed a production asset: ' + name)
            sql = {'link_report': package['link_report'], 'harness_sources': sources}
            parameters = dict(parameters, faultSource='control-worker')
            report['parameters'] = parameters
        else:
            receipt = startup(config, longjmp, 'build', 'soak', STANDARD)
            report['build_receipt'] = str(receipt)
            sql = read_json(receipt)
            shutil.copytree(verify(config, receipt), site)
        link = read_json(Path(sql['link_report']))
        if link['source_snapshot'] != source_snapshot(config.root, link['extensions']):
            raise WasmError('Endurance kernel inputs do not match the current candidate')
        frozen = {str(path.relative_to(site)): digest(path) for path in site.rglob('*') if path.is_file()}
        report['site'] = str(site); report['assets'] = frozen
        run([tools['node'], str(config.root / 'tests/wasm/soak/resources.test.mjs')],
            current / 'resource-gate.log', env, config.root)
        run([tools['node'], str(config.root / 'tests/wasm/soak/processes.test.mjs')],
            current / 'process-accounting.log', env, config.root)
        write_json(current / 'report.json', report)
        query = urlencode(dict(scenario='soak', seconds=parameters['seconds'], seed=parameters['seed'],
                               rounds=parameters['rounds'], reopen=parameters['reopenEvery'],
                               faultSource=parameters.get('faultSource', 'native-pthread')))
        with serve(site, 'sql.html') as url:
            for engine in engines:
                case = current / engine; case.mkdir()
                native = bool(engine == 'webkit' and native_webkit or engine == 'firefox' and platform.system() == 'Linux')
                driver = 'native.mjs' if native else 'browser.mjs'
                command = [tools['node'], str(config.root / 'tests/wasm/soak' / driver),
                    '--root', str(config.root), '--url', url + '?' + query, '--engine', engine,
                    '--output', str(case / 'browser.json'), '--seconds', str(parameters['seconds']),
                    '--growth-mib', str(growth_mib)]
                launcher = config.deps / 'browser-libs/launch-webkit.sh'
                if engine == 'webkit' and native_webkit:
                    command += ['--site', str(site), '--executable', str(native_webkit)]
                elif native:
                    command += ['--site', str(site)]
                elif engine == 'webkit' and launcher.is_file(): command += ['--executable', str(launcher)]
                elif engine == 'chromium' and config.browser_override: command += ['--executable', config.browser_override]
                print(f"wasm soak: {engine}, {parameters['seconds']}s, seed {parameters['seed']}", flush=True)
                begin = time.monotonic()
                row = {'status': 'failed', 'report': str(case / 'browser.json')}
                try:
                    with tempfile.TemporaryDirectory(prefix='pgm-soak-') as temporary:
                        run(command + ['--profile', str(Path(temporary) / 'profile')], case / 'browser.log',
                            dict(browser_environment(config, env), TMPDIR=temporary), config.root,
                            timeout=max(parameters['seconds'] + 660, parameters['rounds'] * 30 + 660))
                    browser = read_json(case / 'browser.json'); validate(browser, parameters)
                    row.update(status='passed', complete=browser['complete'], resourceGate=browser['resourceGate'])
                except Exception as error: row['error'] = str(error)
                row['elapsedSeconds'] = time.monotonic() - begin
                report['engines'][engine] = row; write_json(current / 'report.json', report)
        if link['source_snapshot'] != source_snapshot(config.root, link['extensions']):
            raise WasmError('Kernel inputs changed during the long run')
        for name, expected in {**runner_sources, **sql['harness_sources']}.items():
            if digest(config.root / name) != expected: raise WasmError('Soak input changed: ' + name)
        if any(digest(site / name) != expected for name, expected in frozen.items()):
            raise WasmError('Frozen soak assets changed')
        if native_webkit and digest(native_receipt) != report['nativeBrowserReceipt']['sha256']:
            raise WasmError('Native WebKit receipt changed during the long run')
        if package_receipt and production_package(config, package_receipt) != package:
            raise WasmError('Production package receipt changed during endurance')
        if set(report['engines']) != set(engines) or any(row['status'] != 'passed' for row in report['engines'].values()):
            raise WasmError('Mixed workload browser matrix failed')
        report['status'] = 'passed'
    except Exception as error:
        report['error'] = str(error); raise
    finally:
        report['elapsedSeconds'] = time.monotonic() - started
        write_json(current / 'report.json', report)
        write_json(config.build / 'wasm/reports/soak.json', {'status': report['status'], 'run_report': str(current / 'report.json')})
        print(f"wasm soak: {report['status']}; {current / 'report.json'}", flush=True)
    return current / 'report.json'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, required=True)
    for name in ('build-dir', 'deps-dir', 'sdk-dir', 'profile'): parser.add_argument('--' + name, type=Path)
    parser.add_argument('--target', default='wasm32-emscripten-pthreads')
    parser.add_argument('--mode', default='debug', choices=('debug', 'release'))
    parser.add_argument('--node', default=''); parser.add_argument('--browser-executable', default='')
    parser.add_argument('--longjmp', default='emscripten'); parser.add_argument('--jobs', type=int, default=2)
    parser.add_argument('--engines', default='chromium,firefox,webkit')
    parser.add_argument('--seconds', type=int, default=7200); parser.add_argument('--seed', type=int, default=1)
    parser.add_argument('--rounds', type=int, default=0); parser.add_argument('--reopen-every', type=int, default=50)
    parser.add_argument('--growth-mib', type=int, default=128)
    parser.add_argument('--package-report', type=Path)
    args = parser.parse_args()
    try: check(configuration(args), args.longjmp, args.engines.split(','),
        dict(seconds=args.seconds, seed=args.seed, rounds=args.rounds, reopenEvery=args.reopen_every), args.growth_mib, args.package_report)
    except Exception as error: parser.exit(2, 'wasm soak: ' + str(error).splitlines()[0] + '\n')


if __name__ == '__main__': main()
