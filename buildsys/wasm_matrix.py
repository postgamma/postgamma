#!/usr/bin/env python3
"""Measure the installed browser package; preserve exploratory engine failures."""
from __future__ import annotations
import argparse
from pathlib import Path
import platform
import statistics
import time
from wasm_package import consumer, verify_tree
from wasm_toolchain import (WasmError, configuration, digest, read_json, run_id,
                            sdk_tools, write_json)


def latest_package(config):
    candidates = []
    base = config.build / 'wasm' / config.target / config.mode
    for pointer in base.glob('**/reports/package.json'):
        value = read_json(pointer)
        path = Path(value['run_report'])
        if value['status'] == 'passed' and path.is_file():
            candidates.append(path)
    if not candidates:
        raise WasmError('run make wasm-package-check before measuring the package')
    return max(candidates, key=lambda p: p.stat().st_mtime_ns)


def measurements(browser):
    rows = []
    for case in browser.get('cases', []):
        probe = case.get('probe', {})
        if case.get('status') != 'passed' or probe.get('status') != 'passed':
            continue
        times = probe['timings']
        queries = sorted(times['queryMs'])
        rows.append({'storage': case['storage'], 'timings': times,
            'queryMedianMs': statistics.median(queries),
            'queryP95Ms': queries[max(0, (95 * len(queries) + 99) // 100 - 1)],
            'querySamples': len(queries),
            'peakLinearMemoryBytes': max(m['peakMemory'] for m in probe['metrics']),
            'peakPthreads': max(m['peakThreads'] for m in probe['metrics'])})
    return rows


def matrix(config, package_receipt=None):
    package_receipt = package_receipt or latest_package(config)
    package = read_json(package_receipt)
    if package['status'] != 'passed' or not package.get('consumer_validated'):
        raise WasmError('matrix requires an independently validated production package')
    archive = Path(package['archive']['path'])
    if digest(archive) != package['archive']['sha256']:
        raise WasmError('package archive changed since validation')
    if verify_tree(Path(package['package']), str(config.root)) != package['staged']:
        raise WasmError('package tree changed since validation')
    for name, checksum in package['sources'].items():
        if name.startswith('wasm/src/') and digest(config.root/name) != checksum:
            raise WasmError('package API source changed; rebuild the package')
    tools, env = sdk_tools(config)
    current = config.build/'wasm/matrix'/run_id(); current.mkdir(parents=True)
    sources = [*sorted((config.root/'tests/wasm/package').glob('*')),
               config.root/'buildsys/wasm_matrix.py', config.root/'buildsys/wasm_package.py']
    sources = {str(p.relative_to(config.root)): digest(p) for p in sources if p.is_file()}
    report = {'kind':'postgamma.wasm-package-matrix', 'status':'failed',
        'harness_sources':sources,
        'package_receipt':str(package_receipt), 'package_receipt_sha256':digest(package_receipt),
        'archive':package['archive'], 'unpackedBytes':package['staged']['bytes'],
        'host':{'system':platform.system(), 'machine':platform.machine()},
        'requiredEngines':['chromium'], 'engines':{},
        'measurementScope':'Debug preview, local HTTP with isolation headers, one create/reopen and 20 warm SQL samples per profile. Shared linear memory excludes JS heap and VM/RSS. Linux WebKit is not Safari.'}
    begin = time.monotonic()
    try:
        for engine in ('chromium','firefox','webkit'):
            print('wasm package matrix: ' + engine, flush=True)
            started = time.monotonic()
            row = {'status':'failed'}
            report['engines'][engine] = row
            try:
                row.update(consumer(config, tools, env, archive, current, engine, False, 'all'))
                row['measurements'] = measurements(row['browser'])
            except Exception as error:
                row['error'] = str(error)
                evidence = current/('browser-' + engine + '.json')
                if evidence.is_file():
                    row['browser'] = read_json(evidence)
                    row['measurements'] = measurements(row['browser'])
            row['elapsedSeconds'] = time.monotonic() - started
            write_json(current/'report.json', report)
        report['observedFailures'] = [engine for engine,row in report['engines'].items() if row['status']!='passed']
        if any(report['engines'][engine]['status']!='passed' for engine in report['requiredEngines']):
            raise WasmError('required Chromium package profile failed')
        if any(digest(config.root/name) != checksum for name,checksum in sources.items()):
            raise WasmError('matrix harness changed during validation')
        report['status']='passed'
    except Exception as error:
        report['error']=str(error)
        raise
    finally:
        report['elapsedSeconds']=time.monotonic()-begin
        write_json(current/'report.json',report)
        write_json(config.build/'wasm/reports/matrix.json',{'status':report['status'],'run_report':str(current/'report.json')})
        if report.get("observedFailures"):
            print("wasm package matrix: failed exploratory engines: " + ", ".join(report["observedFailures"]), flush=True)
        print(f"wasm package matrix: required profile {report['status']}; {current/'report.json'}",flush=True)
    return current/'report.json'


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root',type=Path,required=True)
    for name in ('build-dir','deps-dir','sdk-dir','profile'): parser.add_argument('--'+name,type=Path)
    parser.add_argument('--target',default='wasm32-emscripten-pthreads')
    parser.add_argument('--mode',default='debug',choices=('debug','release'))
    parser.add_argument('--node',default='');parser.add_argument('--browser-executable',default='')
    parser.add_argument('--longjmp',default='emscripten');parser.add_argument('--jobs',type=int,default=2)
    parser.add_argument('--package-report',type=Path)
    args=parser.parse_args()
    try: matrix(configuration(args),args.package_report)
    except Exception as error: parser.exit(2,'wasm package matrix: '+str(error).splitlines()[0]+'\n')

if __name__=='__main__': main()
