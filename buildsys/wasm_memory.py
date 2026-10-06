#!/usr/bin/env python3
"""Attribute browser/native memory; diagnostic receipts never qualify release gates."""
import argparse
from pathlib import Path
import shutil
import tempfile
from urllib.parse import urlencode

from wasm import serve
from wasm_browsers import description
from wasm_contrib import STANDARD
from wasm_features import verify
from wasm_sql import startup
from wasm_toolchain import (browser_environment, configuration, digest, read_json,
                            run, run_id, sdk_tools, write_json)


def workers(config, args, folder, report):
    tools, env = sdk_tools(config)
    metadata = description(config, tools, env)
    executable = args.browser_executable
    if not executable and args.engine == 'webkit':
        launcher = config.deps / 'browser-libs/launch-webkit.sh'
        if launcher.is_file(): executable = str(launcher)
    report['tools'] = metadata
    output = folder / 'browser.json'; report['browserReport'] = str(output)
    command = [tools['node'], str(config.root / 'tests/wasm/soak/worker-memory.mjs'),
               '--root', str(config.root), '--engine', args.engine, '--output', str(output),
               '--iterations', str(args.iterations), '--waves', str(args.waves),
               '--idle', str(args.idle_seconds), '--seconds', str(args.seconds)]
    if executable: command += ['--executable', executable]
    print('Worker memory diagnostic: ' + str(folder), flush=True)
    run(command, folder / 'browser.log', browser_environment(config, env), config.root,
        timeout=args.seconds + 30)
    report['status'] = read_json(output)['status']
    if report['status'] != 'diagnostic-completed': raise RuntimeError('Worker diagnostic did not complete')


def diagnose(config, args):
    folder = config.build / 'wasm/memory' / run_id(); folder.mkdir(parents=True)
    report = {'kind': 'postgamma.wasm-memory-diagnostic', 'status': 'failed', 'productionAcceptance': False,
              'workload': args.workload, 'parameters': vars(args).copy()}
    report['parameters'] = {k: str(v) if isinstance(v, Path) else v for k, v in report['parameters'].items()}
    inputs = ['buildsys/wasm_memory.py', 'tests/wasm/soak/memory.mjs', 'tests/wasm/soak/processes.mjs',
              'tests/wasm/soak/browser.mjs', 'tests/wasm/soak/opfs-browser.mjs',
              'tests/wasm/soak/opfs-memory.html', 'tests/wasm/soak/opfs-memory.js',
              'tests/wasm/soak/worker-memory.mjs', 'tests/wasm/soak/worker-memory.js']
    report['sources'] = {name: digest(config.root / name) for name in inputs}
    try:
        if args.workload == 'workers':
            workers(config, args, folder, report)
            for name, expected in report['sources'].items():
                if digest(config.root / name) != expected: raise RuntimeError('Diagnostic source changed: ' + name)
            return
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
    parser.add_argument('--workload', choices=('soak', 'opfs-churn', 'opfs-reuse', 'workers'), default='soak')
    parser.add_argument('--engine', choices=('chromium', 'firefox', 'webkit'), default='chromium')
    parser.add_argument('--waves', type=int, default=3); parser.add_argument('--idle-seconds', type=int, default=60)
    parser.add_argument('--iterations', type=int, default=2000); parser.add_argument('--seconds', type=int, default=1200)
    parser.add_argument('--seed', type=int, default=20261003); parser.add_argument('--reopen-every', type=int, default=50)
    args = parser.parse_args()
    if args.iterations < 1 or args.iterations > 1000000: parser.error('iterations must be between 1 and 1000000')
    if args.engine != 'chromium' and args.workload != 'workers': parser.error('Only workers supports non-Chromium diagnostics')
    if not 2 <= args.waves <= 10: parser.error('waves must be between 2 and 10')
    if not 30 <= args.idle_seconds <= 300: parser.error('idle-seconds must be between 30 and 300')
    if not 1 <= args.seconds <= 28800: parser.error('seconds must be between 1 and 28800')
    if args.workload == 'workers' and (args.receipt or args.frozen_receipt): parser.error('workers does not load a kernel receipt')
    try: diagnose(configuration(args), args)
    except Exception as error: parser.exit(2, 'memory diagnostic: ' + str(error).splitlines()[0] + '\n')


if __name__ == '__main__': main()
