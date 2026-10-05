#!/usr/bin/env python3
"""Attribute browser/native memory; diagnostic receipts never qualify release gates."""
import argparse
from pathlib import Path
import shutil
import tempfile
from urllib.parse import urlencode

from wasm import serve
from wasm_contrib import STANDARD
from wasm_features import verify
from wasm_sql import startup
from wasm_toolchain import (browser_environment, configuration, digest, read_json,
                            run, run_id, sdk_tools, write_json)


def diagnose(config, args):
    folder = config.build / 'wasm/memory' / run_id(); folder.mkdir(parents=True)
    report = {'kind': 'postgamma.wasm-memory-diagnostic', 'status': 'failed',
              'workload': args.workload, 'parameters': vars(args).copy()}
    report['parameters'] = {k: str(v) if isinstance(v, Path) else v for k, v in report['parameters'].items()}
    inputs = ['buildsys/wasm_memory.py', 'tests/wasm/soak/memory.mjs', 'tests/wasm/soak/processes.mjs',
              'tests/wasm/soak/browser.mjs', 'tests/wasm/soak/opfs-browser.mjs',
              'tests/wasm/soak/opfs-memory.html', 'tests/wasm/soak/opfs-memory.js']
    report['sources'] = {name: digest(config.root / name) for name in inputs}
    try:
        if args.frozen_receipt:
            frozen = read_json(args.frozen_receipt)
            origin = Path(frozen['site'])
            for name, expected in frozen['assets'].items():
                if digest(origin / name) != expected: raise RuntimeError('Frozen asset changed: ' + name)
            report['frozenReceipt'] = str(args.frozen_receipt)
            report['frozenReceiptSha256'] = digest(args.frozen_receipt)
            report['buildReceipt'] = frozen['build_receipt']
        else:
            receipt = args.receipt or startup(config, args.longjmp, 'build', 'soak', STANDARD)
            report['buildReceipt'] = str(receipt)
            origin = verify(config, receipt)
        site = folder / 'site'; shutil.copytree(origin, site)
        for name in ('opfs-memory.html', 'opfs-memory.js'):
            shutil.copyfile(config.root / 'tests/wasm/soak' / name, site / name)
        report['assets'] = {str(p.relative_to(site)): digest(p) for p in site.rglob('*') if p.is_file()}
        tools, env = sdk_tools(config)
        output = folder / 'browser.json'
        report['browserReport'] = str(output)
        with serve(site, 'sql.html' if args.workload == 'soak' else 'opfs-memory.html') as url:
            with tempfile.TemporaryDirectory(prefix='pgm-memory-') as temporary:
                if args.workload == 'soak':
                    url += '?' + urlencode(dict(scenario='soak', seed=args.seed, seconds=args.seconds,
                                                rounds=args.iterations, reopen=args.reopen_every))
                    command = [tools['node'], str(config.root / 'tests/wasm/soak/browser.mjs'),
                        '--root', str(config.root), '--url', url, '--profile', temporary,
                        '--output', str(output), '--seconds', str(args.seconds), '--memory-diagnostics']
                else:
                    command = [tools['node'], str(config.root / 'tests/wasm/soak/opfs-browser.mjs'),
                        str(config.root), url, temporary, str(output), args.workload, str(args.iterations)]
                print('memory diagnostic: ' + str(folder), flush=True)
                run(command, folder / 'browser.log', browser_environment(config, env), config.root,
                    timeout=max(args.seconds + 660, args.iterations * 30 + 660))
        report['status'] = read_json(output)['status']
        if report['status'] != 'diagnostic-completed': raise RuntimeError('Memory diagnostic did not complete')
        for name, expected in report['sources'].items():
            if digest(config.root / name) != expected: raise RuntimeError('Diagnostic source changed: ' + name)
        for name, expected in report['assets'].items():
            if digest(site / name) != expected: raise RuntimeError('Diagnostic asset changed: ' + name)
    except Exception as error:
        report['status'] = 'failed'; report['error'] = str(error); raise
    finally:
        write_json(folder / 'report.json', report)
        print('memory diagnostic: ' + report['status'] + '; ' + str(folder / 'report.json'), flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, required=True)
    for name in ('build-dir', 'deps-dir', 'sdk-dir', 'profile', 'receipt', 'frozen-receipt'):
        parser.add_argument('--' + name, type=Path)
    parser.add_argument('--target', default='wasm32-emscripten-pthreads')
    parser.add_argument('--mode', default='release', choices=('debug', 'release'))
    parser.add_argument('--longjmp', default='emscripten'); parser.add_argument('--jobs', type=int, default=2)
    parser.add_argument('--node', default=''); parser.add_argument('--browser-executable', default='')
    parser.add_argument('--workload', choices=('soak', 'opfs-churn', 'opfs-reuse'), default='soak')
    parser.add_argument('--iterations', type=int, default=2000); parser.add_argument('--seconds', type=int, default=1200)
    parser.add_argument('--seed', type=int, default=20261003); parser.add_argument('--reopen-every', type=int, default=50)
    args = parser.parse_args()
    if args.iterations < 1 or args.iterations > 1000000: parser.error('iterations must be between 1 and 1000000')
    try: diagnose(configuration(args), args)
    except Exception as error: parser.exit(2, 'memory diagnostic: ' + str(error).splitlines()[0] + '\n')


if __name__ == '__main__': main()
