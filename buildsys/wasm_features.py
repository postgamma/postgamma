#!/usr/bin/env python3
"""Exercise archive, extension and parallel SQL scenarios in the browser matrix."""
import argparse
from pathlib import Path
import tempfile

from wasm import serve
from wasm_sql import startup
from wasm_link import source_snapshot
from wasm_contrib import selection
from wasm_toolchain import (WasmError, browser_environment, configuration, digest,
                            read_json, run, run_id, sdk_tools, write_json)


def parallel_plans(row):
    """Require actual two-worker execution, including after persistent reopen."""
    expected = {prefix + '-' + case for prefix in ('ephemeral', 'opfs') for case in
                ('scan', 'aggregate', 'hash-join', 'gather-merge', 'worker-guc',
                 'snapshot', 'leader-excluded', 'reuse-after-errors')} | {'opfs-reopen'}
    plans = [event for event in row.get('events', []) if event.get('kind') == 'parallel-plan']
    if len(plans) != len(expected) or {event.get('label') for event in plans} != expected:
        raise WasmError('Missing two-worker execution plans')

    def nodes(plan):
        return [plan] + [node for child in plan.get('Plans', []) for node in nodes(child)]

    for event in plans:
        gathers = [node for node in nodes(event['plan']) if node.get('Node Type') in ('Gather', 'Gather Merge')]
        if not gathers or any(node.get('Workers Planned') != 2 or node.get('Workers Launched') != 2 for node in gathers):
            raise WasmError('EXPLAIN must show two planned and two launched workers: ' + event['label'])
    return len(plans)


def verify(config, receipt):
    sql = read_json(receipt)
    link = read_json(Path(sql['link_report']))
    if sql['status'] != 'passed' or link['status'] != 'passed' or not link['kernel_linked']:
        raise WasmError('Feature checks require a linked kernel')
    if link['source_snapshot'] != source_snapshot(config.root, link['extensions']):
        raise WasmError('Feature kernel inputs changed')
    for base, key in ((config.root, 'harness_sources'), (Path(sql['site']), 'harness')):
        for name, expected in sql[key].items():
            if digest(base / name) != expected:
                raise WasmError('Feature source or artifact changed: ' + name)
    return Path(sql['site'])


def matrix(config, longjmp, scenario, extensions, engines):
    if not engines or len(set(engines)) != len(engines) or any(e not in ('chromium', 'firefox', 'webkit') for e in engines):
        raise WasmError('Invalid feature browser selection')
    current = config.build / 'wasm' / scenario / run_id(); current.mkdir(parents=True)
    report = {'kind': 'postgamma.wasm-feature-matrix', 'status': 'failed',
              'scenario': scenario, 'mode': config.mode, 'engines': {}}
    sources = {name: digest(config.root / name) for name in ('buildsys/wasm_features.py',
        'tests/wasm/browser/run.mjs', 'tests/wasm/browser/diagnostics.mjs', 'tests/wasm/browser/evidence.mjs')}
    if scenario == 'archive': sources['tests/wasm/storage/archive.mjs'] = digest(config.root / 'tests/wasm/storage/archive.mjs')
    report['runner_sources'] = sources
    try:
        receipt = startup(config, longjmp, 'build', scenario, extensions)
        report['build_receipt'] = str(receipt)
        site = verify(config, receipt)
        tools, env = sdk_tools(config)
        if scenario == 'archive':
            run([tools['node'], str(config.root / 'tests/wasm/storage/archive.mjs'),
                 str(site / 'src/storage/archive.js')], current / 'framing.log', env, config.root)
        with serve(site, 'sql.html') as url:
            for engine in engines:
                case = current / engine; case.mkdir()
                timeout = 240 if scenario == 'parallel' else 600
                command = [tools['node'], str(config.root / 'tests/wasm/browser/run.mjs'),
                    '--root', str(config.root), '--url', url + '?scenario=' + scenario,
                    '--engine', engine, '--output', str(case / 'browser.json'), '--timeout-ms', str(timeout * 1000)]
                launcher = config.deps / 'browser-libs/launch-webkit.sh'
                if engine == 'webkit' and launcher.is_file(): command += ['--executable', str(launcher)]
                elif engine == 'chromium' and config.browser_override: command += ['--executable', config.browser_override]
                print(f'wasm {scenario}: {engine}', flush=True)
                try:
                    with tempfile.TemporaryDirectory(prefix='pgm-feature-') as temporary:
                        run(command + ['--profile', str(Path(temporary) / 'profile')], case / 'browser.log',
                            dict(browser_environment(config, env), TMPDIR=temporary), config.root, timeout=timeout + 60)
                    row = read_json(case / 'browser.json')
                    if row['status'] != 'passed' or not row.get('probe', {}).get('checks'):
                        raise WasmError('Feature browser evidence is incomplete')
                    if scenario == 'archive':
                        required = {'archive-active-transaction-rejected', 'archive-active-request-rejected',
                            'archive-ephemeral-to-opfs', 'archive-opfs-to-ephemeral-clone', 'archive-clone-independent-writes',
                            'archive-existing-destination-preserved', 'archive-checksum', 'archive-truncation',
                            'archive-trailing-data', 'archive-build-identity', 'archive-path-traversal',
                            'archive-cancelled-restore-not-openable', 'archive-cancelled-export-reopen', 'archive-concurrent-close',
                            'archive-invalid-cluster-quarantined'}
                        if not required <= set(row['probe']['checks']): raise WasmError('Missing archive checks')
                    if scenario == 'contrib':
                        required = {'contrib-extension-ddl-rollback', 'contrib-pg-trgm-gin', 'contrib-pg-trgm-gist',
                            'contrib-hstore-gin', 'contrib-null-unicode', 'contrib-four-session-guc-isolation',
                            'contrib-local-guc-rollback', 'contrib-session-reuse', 'contrib-opfs-reopen', 'contrib-archive-roundtrip'}
                        if not required <= set(row['probe']['checks']): raise WasmError('Missing contrib checks')
                    if scenario == 'parallel':
                        required = {'parallel-two-workers', 'parallel-leader-excluded',
                            'parallel-scan-values', 'parallel-aggregate-values', 'parallel-hash-join-values',
                            'parallel-gather-merge-values', 'parallel-guc-isolation', 'parallel-snapshot',
                            'parallel-cancel', 'parallel-peer-progress', 'parallel-worker-budget-fallback', 'parallel-timeout', 'parallel-worker-error',
                            'parallel-reuse', 'parallel-ephemeral', 'parallel-opfs', 'parallel-opfs-reopen', 'parallel-resource-release'}
                        if not required <= set(row['probe']['checks']): raise WasmError('Missing parallel SQL checks')
                        parallel_plans(row)
                    for module in row['probe']['metrics']:
                        if module.get('resources') != dict(locks=0, shmem=0, dsm=0, pipes=0) or module.get('runningWorkers') != 0:
                            raise WasmError('Feature module did not release native resources')
                    report['engines'][engine] = {'status': 'passed', 'report': str(case / 'browser.json'),
                        'checks': row['probe']['checks'], 'elapsedMs': row['elapsedMs']}
                except Exception as error:
                    report['engines'][engine] = {'status': 'failed', 'report': str(case / 'browser.json'), 'error': str(error)}
                write_json(current / 'report.json', report)
        verify(config, receipt)
        if any(digest(config.root / name) != expected for name, expected in sources.items()):
            raise WasmError('Feature runner changed during validation')
        if any(row['status'] != 'passed' for row in report['engines'].values()):
            raise WasmError('Feature browser matrix failed')
        report['status'] = 'passed'
    except Exception as error:
        report['error'] = str(error); raise
    finally:
        write_json(current / 'report.json', report)
        write_json(config.build / 'wasm/reports' / (scenario + '.json'),
                   {'status': report['status'], 'run_report': str(current / 'report.json')})
        print(f"wasm {scenario}: {report['status']}; {current / 'report.json'}", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, required=True)
    for name in ('build-dir', 'deps-dir', 'sdk-dir', 'profile'): parser.add_argument('--' + name, type=Path)
    parser.add_argument('--target', default='wasm32-emscripten-pthreads')
    parser.add_argument('--mode', default='debug', choices=('debug', 'release'))
    parser.add_argument('--node', default=''); parser.add_argument('--browser-executable', default='')
    parser.add_argument('--longjmp', default='emscripten'); parser.add_argument('--jobs', type=int, default=2)
    parser.add_argument('--scenario', default='archive', choices=('archive', 'contrib', 'parallel', 'statements', 'controls', 'types', 'streams'))
    parser.add_argument('--extensions', default='standard')
    parser.add_argument('--engines', default='chromium,firefox,webkit')
    args = parser.parse_args()
    try: matrix(configuration(args), args.longjmp, args.scenario, selection(args.extensions), args.engines.split(','))
    except Exception as error: parser.exit(2, 'wasm features: ' + str(error).splitlines()[0] + '\n')


if __name__ == '__main__': main()
