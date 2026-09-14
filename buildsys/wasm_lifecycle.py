#!/usr/bin/env python3
"""Reproduce OPFS error recovery in every required browser, including repeated fatals."""
from __future__ import annotations

import argparse
from pathlib import Path
import tempfile

from wasm import serve
from wasm_sql import startup
from wasm_toolchain import (WasmError, browser_environment, configuration, digest,
                            read_json, run, run_id, sdk_tools, write_json)

SCENARIOS = ('close-audit', 'open-failed', 'fatal-opfs-trap', 'fatal-opfs-abort')
ENGINES = ('chromium', 'firefox', 'webkit')


def verify_inputs(config, sql):
    if sql.get('status') != 'passed' or not sql.get('sql_validated') or sql.get('action') != 'check':
        raise WasmError('lifecycle matrix requires a passing SQL check receipt')
    for base, key in ((Path(sql['site']), 'harness'), (config.root, 'harness_sources')):
        for name, checksum in sql[key].items():
            if digest(base / name) != checksum:
                raise WasmError('lifecycle source or asset changed: ' + name)
    manifest = read_json(Path(sql['site']) / 'postgamma.assets.json')
    if manifest['mode'] != config.mode:
        raise WasmError('lifecycle receipt mode differs from the requested mode')


def validate_case(row, scenario, repetitions):
    if row.get('status') != 'passed' or row.get('probe', {}).get('status') != 'passed':
        raise WasmError('lifecycle case failed: ' + scenario)
    events = row.get('events', [])
    timings = [e['elapsedMs'] for e in events if e.get('kind') == 'recovery-timing']
    expected = repetitions if scenario.startswith('fatal-') else 1
    if len(timings) != expected or any(t < 0 or t >= 30000 for t in timings):
        raise WasmError('missing or out-of-budget lifecycle recovery: ' + scenario)
    if scenario.startswith('fatal-'):
        rounds = [e for e in events if e.get('kind') == 'fatal-iteration']
        fault = 1 if scenario.endswith('trap') else 2
        if [e['iteration'] for e in rounds] != list(range(1, repetitions + 1)) or any(e['fault'] != fault for e in rounds):
            raise WasmError('incomplete fatal repetition evidence: ' + scenario)


def matrix(config, longjmp, repetitions=20, sql_receipt=None):
    if repetitions < 1 or repetitions > 100:
        raise WasmError('lifecycle repetitions must be between 1 and 100')
    sql_receipt = sql_receipt or startup(config, longjmp, 'check', 'lifecycle')
    sql_receipt = sql_receipt.resolve()
    sql = read_json(sql_receipt)
    verify_inputs(config, sql)
    tools, env = sdk_tools(config)
    current = config.build / 'wasm/lifecycle' / run_id()
    current.mkdir(parents=True)
    sources = {name: digest(config.root / name) for name in (
        'buildsys/wasm_lifecycle.py', 'tests/wasm/browser/run.mjs', 'tests/wasm/browser/evidence.mjs')}
    report = {'kind': 'postgamma.wasm-lifecycle-matrix', 'status': 'failed',
        'sql_receipt': str(sql_receipt), 'sql_receipt_sha256': digest(sql_receipt),
        'harness_sources': sources, 'mode': config.mode, 'repetitions': repetitions,
        'fixtureBudgetMs': 90000, 'recoveryBudgetMs': 30000, 'engines': {},
        'scope': 'Fresh runs in Chromium, Firefox and Linux WebKit. Fatal iterations reuse durable data '
                 'with fresh modules. Immediate reopen has no sleep or retry; initial creation is timed separately. '
                 'Repeated success is empirical evidence, not a synchronous pthread termination guarantee.'}
    try:
        with serve(Path(sql['site']), 'sql.html') as url:
            for engine in ENGINES:
                report['engines'][engine] = {}
                for scenario in SCENARIOS:
                    case = current / (engine + '-' + scenario)
                    case.mkdir()
                    query = '?scenario=' + scenario
                    timeout = 90000
                    if scenario.startswith('fatal-'):
                        query += '&repetitions=' + str(repetitions)
                    command = [tools['node'], str(config.root / 'tests/wasm/browser/run.mjs'),
                        '--root', str(config.root), '--url', url + query, '--engine', engine,
                        '--output', str(case / 'browser.json'), '--timeout-ms', str(timeout)]
                    if scenario.startswith('fatal-'):
                        command += ['--repetitions', str(repetitions)]
                    launcher = config.deps / 'browser-libs/launch-webkit.sh'
                    if engine == 'webkit' and launcher.is_file():
                        command += ['--executable', str(launcher)]
                    elif engine == 'chromium' and config.browser_override:
                        command += ['--executable', config.browser_override]
                    print('wasm lifecycle: ' + engine + ' ' + scenario, flush=True)
                    try:
                        with tempfile.TemporaryDirectory(prefix='pgm-lifecycle-') as temporary:
                            run(command + ['--profile', str(Path(temporary) / 'profile')],
                                case / 'browser.log', dict(browser_environment(config, env), TMPDIR=temporary),
                                config.root, timeout=timeout / 1000 * (repetitions + 1 if scenario.startswith('fatal-') else 1) + 30)
                        row = read_json(case / 'browser.json')
                        validate_case(row, scenario, repetitions)
                        report['engines'][engine][scenario] = row
                    except Exception as error:
                        row = read_json(case / 'browser.json') if (case / 'browser.json').is_file() else {}
                        report['engines'][engine][scenario] = {**row, 'status': 'failed', 'error': str(error)}
                    finally:
                        write_json(current / 'report.json', report)
        verify_inputs(config, sql)
        if any(digest(config.root / name) != checksum for name, checksum in sources.items()):
            raise WasmError('lifecycle runner changed during validation')
        failures = [engine + '/' + scenario for engine, cases in report['engines'].items()
                    for scenario, row in cases.items() if row['status'] != 'passed']
        if failures:
            raise WasmError('lifecycle failures: ' + ', '.join(failures))
        report['status'] = 'passed'
    except Exception as error:
        report['error'] = str(error)
        raise
    finally:
        write_json(current / 'report.json', report)
        write_json(config.build / 'wasm/reports/lifecycle.json', {'status': report['status'], 'run_report': str(current / 'report.json')})
        print(f"wasm lifecycle: {report['status']}; {current / 'report.json'}", flush=True)
    return current / 'report.json'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, required=True)
    for name in ('build-dir', 'deps-dir', 'sdk-dir', 'profile'):
        parser.add_argument('--' + name, type=Path)
    parser.add_argument('--target', default='wasm32-emscripten-pthreads')
    parser.add_argument('--mode', default='debug', choices=('debug', 'release'))
    parser.add_argument('--node', default=''); parser.add_argument('--browser-executable', default='')
    parser.add_argument('--longjmp', default='emscripten'); parser.add_argument('--jobs', type=int, default=2)
    parser.add_argument('--repetitions', type=int, default=20)
    parser.add_argument('--sql-report', type=Path)
    args = parser.parse_args()
    try:
        matrix(configuration(args), args.longjmp, args.repetitions, args.sql_report)
    except Exception as error:
        parser.exit(2, 'wasm lifecycle: ' + str(error).splitlines()[0] + '\n')


if __name__ == '__main__':
    main()
