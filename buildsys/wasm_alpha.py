#!/usr/bin/env python3
"""Qualify the optimized npm package with full gates and production endurance."""
from __future__ import annotations
import argparse
from pathlib import Path
import time
from wasm_ci import run_suite
from wasm_soak import check as soak, production_package, ENGINES
from wasm_toolchain import WasmError, capture, configuration, digest, read_json, run_id, write_json


def validate_endurance(report, archive, seconds):
    if (report.get('status') != 'passed' or report.get('mode') != 'release' or
            set(report.get('engines', {})) != set(ENGINES) or
            report.get('growthMiB', 129) > 128 or
            report.get('productionPackage', {}).get('archive') != archive or
            report.get('productionPackage', {}).get('bridgeTesting') is not False):
        raise WasmError('Alpha requires passing production-package endurance in all three engines')
    parameters = report.get('parameters', {})
    if parameters.get('seconds', 0) < seconds or parameters.get('rounds') != 0 or parameters.get('faultSource') != 'control-worker':
        raise WasmError('Alpha endurance duration or production failure source is incomplete')
    for engine, row in report['engines'].items():
        if (row.get('status') != 'passed' or row.get('complete', {}).get('elapsedMs', 0) < seconds * 1000 or
                row.get('resourceGate', {}).get('status') != 'passed'):
            raise WasmError('Alpha endurance did not complete: ' + engine)


def qualify(config, longjmp, seconds=7200, seed=20261003):
    if config.mode != 'release' or not 7200 <= seconds <= 28800:
        raise WasmError('Alpha qualification requires release mode and at least 7200 active seconds per engine')
    if not 1 <= seed <= 0xffffffff:
        raise WasmError('Alpha workload seed must be a positive uint32')
    current = config.build / 'wasm/alpha' / run_id(); current.mkdir(parents=True)
    report = {'kind': 'postgamma.wasm-alpha-qualification', 'status': 'failed', 'phase': 'freeze',
              'mode': config.mode, 'secondsPerEngine': seconds, 'seed': seed,
              'publication': 'not-requested', 'release': {'status': 'not-run'}, 'endurance': {'status': 'not-run'}}
    started = time.monotonic()
    try:
        if capture(['git', '-C', str(config.root), 'status', '--porcelain', '--untracked-files=no']):
            raise WasmError('Alpha source must have no tracked changes, including submodules')
        commit = capture(['git', '-C', str(config.root), 'rev-parse', 'HEAD'])
        paths = capture(['git', '-C', str(config.root), 'ls-files', '-z']).split('\0')
        inputs = {name: digest(config.root / name) for name in paths if name and (config.root / name).is_file()}
        report['sourceCommit'] = commit; report['sourceInputs'] = inputs
        report['phase'] = 'release'; write_json(current / 'report.json', report)
        receipt = run_suite(config, 'weekly', longjmp, keep_going=True)
        report['release'] = {'status': 'passed', 'report': str(receipt), 'sha256': digest(receipt)}
        matrix = read_json(Path(read_json(receipt)['matrix_report']))
        package_receipt = Path(matrix['package_receipt'])
        package = production_package(config, package_receipt)
        report['archive'] = package['archive']; report['buildId'] = package['staged']['build_id']
        report['packageReceipt'] = {'path': str(package_receipt), 'sha256': digest(package_receipt)}
        report['phase'] = 'endurance'; write_json(current / 'report.json', report)
        parameters = dict(seconds=seconds, seed=seed, rounds=0, reopenEvery=50)
        endurance = soak(config, longjmp, list(ENGINES), parameters, 128, package_receipt)
        validate_endurance(read_json(endurance), package['archive'], seconds)
        report['endurance'] = {'status': 'passed', 'report': str(endurance), 'sha256': digest(endurance)}
        if capture(['git', '-C', str(config.root), 'rev-parse', 'HEAD']) != commit or capture(
                ['git', '-C', str(config.root), 'status', '--porcelain', '--untracked-files=no']):
            raise WasmError('Alpha source identity changed during qualification')
        if any(digest(config.root / name) != expected for name, expected in inputs.items()):
            raise WasmError('Alpha tracked inputs changed during qualification')
        if production_package(config, package_receipt) != package:
            raise WasmError('Alpha production package changed during qualification')
        report['status'] = 'passed'; report['phase'] = 'completed'
    except Exception as error:
        report['error'] = str(error)
        phase = report['phase']
        if phase in ('release', 'endurance'):
            pointer = config.build / 'wasm/reports' / ('ci.json' if phase == 'release' else 'soak.json')
            report[phase] = dict(read_json(pointer), status='failed') if pointer.is_file() else {'status': 'failed'}
        raise
    finally:
        report['elapsedSeconds'] = time.monotonic() - started
        write_json(current / 'report.json', report)
        write_json(config.build / 'wasm/reports/alpha.json', {'status': report['status'], 'run_report': str(current / 'report.json')})
        print(f"wasm alpha: {report['status']}; {current / 'report.json'}", flush=True)
    return current / 'report.json'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, required=True)
    for name in ('build-dir', 'deps-dir', 'sdk-dir', 'profile'): parser.add_argument('--' + name, type=Path)
    parser.add_argument('--target', default='wasm32-emscripten-pthreads')
    parser.add_argument('--mode', default='release', choices=('debug', 'release'))
    parser.add_argument('--node', default=''); parser.add_argument('--browser-executable', default='')
    parser.add_argument('--longjmp', default='emscripten'); parser.add_argument('--jobs', type=int, default=1)
    parser.add_argument('--seconds', type=int, default=7200); parser.add_argument('--seed', type=int, default=20261003)
    args = parser.parse_args()
    try: qualify(configuration(args), args.longjmp, args.seconds, args.seed)
    except Exception as error: parser.exit(2, 'wasm alpha: ' + str(error).splitlines()[0] + '\n')


if __name__ == '__main__': main()
