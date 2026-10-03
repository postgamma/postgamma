#!/usr/bin/env python3
"""Run pinned PGlite-derived cases against the real PostGamma browser kernel."""
from __future__ import annotations

import argparse
from pathlib import Path
import tempfile
import time

from wasm import serve
from wasm_sql import startup
from wasm_toolchain import (WasmError, browser_environment, configuration, digest,
                            read_json, run, run_id, sdk_tools, write_json)

ENGINES = ('chromium', 'firefox', 'webkit')
STORAGE = ('ephemeral', 'opfs')


def inventory(root):
    directory = root / 'tests/wasm/pglite'
    fixture, upstream = read_json(directory / 'cases.json'), read_json(directory / 'upstream.json')
    cases = fixture['cases']
    ids = [case['id'] for case in cases]
    if fixture.get('schemaVersion') != 1 or not ids or len(ids) != len(set(ids)):
        raise WasmError('empty, duplicate or unsupported PGlite case inventory')
    sources = {row['path']: row for row in upstream['files']}
    listed = [case for row in sources.values() for case in row['cases']]
    if sorted(listed) != sorted(ids):
        raise WasmError('PGlite provenance does not match the executed case inventory')
    for case in cases:
        row = sources.get(case['source'], {})
        if case['id'] not in row.get('cases', ()) or not case.get('upstreamTest'):
            raise WasmError('missing PGlite case provenance: ' + case['id'])
        if bool(case.get('steps')) == bool(case.get('kind')):
            raise WasmError('case must select SQL steps or a custom driver: ' + case['id'])
    return ids, upstream


def verify_inputs(config, receipt):
    from wasm_link import source_snapshot
    sql = read_json(receipt)
    link = read_json(Path(sql['link_report']))
    if sql.get('status') != 'passed' or sql.get('scenario') != 'pglite' or link.get('status') != 'passed' or not link.get('kernel_linked'):
        raise WasmError('PGlite tests require a successful matching kernel build')
    if link.get('extensions') != ['pgvector'] or link.get('source_snapshot') != source_snapshot(config.root, ('pgvector',)):
        raise WasmError('PGlite kernel inputs changed; rebuild before reusing the receipt')
    if link.get('bridge', {}).get('sha256') != digest(config.root / 'wasm/bridge/sql.c'):
        raise WasmError('PGlite SQL bridge changed')
    for base, key in ((config.root, 'harness_sources'), (Path(sql['site']), 'harness')):
        for name, checksum in sql[key].items():
            if digest(base / name) != checksum:
                raise WasmError('PGlite test source or artifact changed: ' + name)
    site = Path(sql['site'])
    manifest = read_json(site / 'postgamma.assets.json')
    if manifest['mode'] != config.mode:
        raise WasmError('PGlite build mode differs from requested mode')
    return site


def validate_case(row, ids, storage):
    probe = row.get('probe', {})
    if row.get('status') != 'passed' or probe.get('status') != 'passed' or probe.get('storage') != storage:
        raise WasmError('browser did not pass the requested PGlite storage profile')
    for cases in (row.get('cases', []), probe.get('cases', [])):
        if [case.get('id') for case in cases] != ids or any(case.get('status') != 'passed' for case in cases):
            raise WasmError('missing, duplicated, reordered or failed PGlite cases')
    if not all(probe.get(key) for key in ('isolated', 'worker', 'sharedMemory')):
        raise WasmError('missing isolated Worker/shared-memory evidence')
    if row.get('pageErrors') or row.get('failedRequests'):
        raise WasmError('browser page or asset errors')
    metrics = probe.get('metrics', [])
    if len(metrics) != 2 or any(m.get('resources') != dict(locks=0, shmem=0, dsm=0, pipes=0) for m in metrics):
        raise WasmError('missing native resource release evidence before and after reopen')


def completed_attempt(row, remaining):
    """Retain finished cases and the active failure; never retry that failure."""
    cases = list(row.get('cases', []))
    if [case.get('id') for case in cases] != remaining[:len(cases)]:
        raise WasmError('browser progress is not a prefix of the requested inventory')
    if len(cases) < len(remaining):
        starts = [event.get('id') for event in row.get('events', []) if event.get('kind') == 'case-start']
        if starts and starts[-1] == remaining[len(cases)]:
            cases.append({'id': starts[-1], 'status': 'failed', 'completed': False,
                          'error': row.get('error', 'browser stopped before case completion'),
                          'failureSnapshot': row.get('failureSnapshot')})
    return cases


def matrix(config, longjmp, engines=ENGINES, storage_profiles=STORAGE, receipt=None):
    ids, upstream = inventory(config.root)
    fixture = read_json(config.root / 'tests/wasm/pglite/cases.json')['cases']
    dependent = {case['id'] for case in fixture if case.get('kind') in ('database-close', 'reopen', 'vector-reopen')}
    if not engines or len(set(engines)) != len(engines) or any(e not in ENGINES for e in engines):
        raise WasmError('invalid PGlite engine selection')
    if not storage_profiles or len(set(storage_profiles)) != len(storage_profiles) or any(s not in STORAGE for s in storage_profiles):
        raise WasmError('invalid PGlite storage selection')
    current = config.build / 'wasm/pglite' / run_id(); current.mkdir(parents=True)
    report = {'kind': 'postgamma.wasm-pglite', 'status': 'failed', 'mode': config.mode,
              'upstream': upstream, 'expected_cases': ids, 'profiles': {},
              'complete_matrix': tuple(engines) == ENGINES and tuple(storage_profiles) == STORAGE,
              'scope': 'Adapted PGlite SQL/API scenarios on the PostGamma kernel. Linux WebKit is not Safari/iOS evidence. '
                       'Large collected results assert the PostGamma limit; iterator transfers are bounded to 16 MiB per case.'}
    sources = {str(p.relative_to(config.root)): digest(p) for p in sorted((config.root / 'tests/wasm/pglite').iterdir()) if p.is_file()}
    sources['tests/wasm/browser/diagnostics.mjs'] = digest(config.root / 'tests/wasm/browser/diagnostics.mjs')
    report['test_sources'] = sources
    try:
        receipt = Path(receipt) if receipt else startup(config, longjmp, 'build', 'pglite', ('pgvector',))
        report['build_receipt'], report['build_receipt_sha256'] = str(receipt), digest(receipt)
        site = verify_inputs(config, receipt)
        tools, env = sdk_tools(config)
        with serve(site, 'pglite.html') as url:
            for engine in engines:
                for storage in storage_profiles:
                    name = engine + '-' + storage
                    profile = current / name; profile.mkdir()
                    command = [tools['node'], str(config.root / 'tests/wasm/pglite/browser.mjs'),
                        '--root', str(config.root), '--engine', engine,
                        '--cases', str(config.root / 'tests/wasm/pglite/cases.json')]
                    launcher = config.deps / 'browser-libs/launch-webkit.sh'
                    if engine == 'webkit' and launcher.is_file(): command += ['--executable', str(launcher)]
                    elif engine == 'chromium' and config.browser_override: command += ['--executable', config.browser_override]
                    print('wasm PGlite: ' + name + f' ({len(ids)} cases)', flush=True)
                    completed, attempts, errors = [], [], []
                    deadline = time.monotonic() + 1200
                    row = {}
                    while len(completed) < len(ids) and deadline - time.monotonic() > 15:
                        offset = len(completed)
                        case = profile / f'attempt-{len(attempts) + 1:02}'; case.mkdir()
                        output = case / 'browser.json'; attempts.append(str(output))
                        try:
                            with tempfile.TemporaryDirectory(prefix='pgm-pglite-') as temporary:
                                run(command + ['--profile', str(Path(temporary) / 'profile'), '--output', str(output),
                                    '--start-index', str(offset), '--url', url + f'?storage={storage}&start={offset}'],
                                    case / 'browser.log', dict(browser_environment(config, env), TMPDIR=temporary),
                                    config.root, timeout=max(1, int(deadline - time.monotonic())))
                            row = read_json(output)
                        except Exception as error:
                            row = read_json(output) if output.is_file() else {}
                            row = {**row, 'status': 'failed', 'error': row.get('error', str(error))}
                            errors.append(row['error'])
                        progress = completed_attempt(row, ids[offset:]); completed.extend(progress)
                        report['profiles'][name] = {**row, 'cases': completed, 'attempt_reports': attempts,
                            'unfinishedCases': ids[len(completed):], 'status': 'failed'}
                        write_json(current / 'report.json', report)
                        # Ordinary fixtures own isolated schemas. A fresh browser
                        # can continue after one stalls, preserving that failure.
                        # Close/reopen cases share state and cannot be resumed.
                        if not progress or progress[-1]['id'] in dependent:
                            break
                        if len(completed) < len(ids):
                            print('wasm PGlite: ' + name + ' continuing in a fresh browser at ' + ids[len(completed)], flush=True)
                    result = {**row, 'cases': completed, 'attempt_reports': attempts,
                              'unfinishedCases': ids[len(completed):], 'status': 'failed'}
                    failed = [case['id'] for case in completed if case['status'] != 'passed']
                    try:
                        if errors or failed or len(completed) != len(ids):
                            raise WasmError('failed cases: ' + ', '.join(failed) + f'; unfinished: {len(ids) - len(completed)}')
                        validate_case(row, ids, storage)
                        result['status'] = 'passed'
                    except Exception as error: result['error'] = str(error)
                    report['profiles'][name] = result
                    write_json(profile / 'report.json', result)
                    write_json(current / 'report.json', report)
                    print(f'wasm PGlite: {name}: {len(completed)-len(failed)} passed, {len(failed)} failed, '
                          f'{len(ids)-len(completed)} unfinished', flush=True)
        verify_inputs(config, receipt)
        if digest(receipt) != report['build_receipt_sha256'] or any(digest(config.root / p) != sha for p, sha in sources.items()):
            raise WasmError('PGlite receipt or test sources changed during validation')
        failed = [name for name, row in report['profiles'].items() if row['status'] != 'passed']
        if failed: raise WasmError('PGlite-derived failures: ' + ', '.join(failed))
        report['status'] = 'passed'
    except Exception as error:
        report['error'] = str(error); raise
    finally:
        write_json(current / 'report.json', report)
        write_json(config.build / 'wasm/reports/pglite.json', {'status': report['status'], 'run_report': str(current / 'report.json')})
        print(f"wasm PGlite: {report['status']}; {current / 'report.json'}", flush=True)
    return current / 'report.json'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, required=True)
    for name in ('build-dir', 'deps-dir', 'sdk-dir', 'profile'): parser.add_argument('--' + name, type=Path)
    parser.add_argument('--target', default='wasm32-emscripten-pthreads')
    parser.add_argument('--mode', default='debug', choices=('debug', 'release'))
    parser.add_argument('--longjmp', default='emscripten'); parser.add_argument('--jobs', type=int, default=2)
    parser.add_argument('--node', default=''); parser.add_argument('--browser-executable', default='')
    parser.add_argument('--engines', default=','.join(ENGINES)); parser.add_argument('--storage', default=','.join(STORAGE))
    parser.add_argument('--build-receipt', type=Path)
    args = parser.parse_args()
    try: matrix(configuration(args), args.longjmp, tuple(args.engines.split(',')), tuple(args.storage.split(',')), args.build_receipt)
    except Exception as error: parser.exit(2, 'wasm PGlite: ' + str(error).splitlines()[0] + '\n')


if __name__ == '__main__': main()
