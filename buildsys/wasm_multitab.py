#!/usr/bin/env python3
"""Validate ownership and interrupted recovery across real same-origin tabs."""
import argparse
from pathlib import Path
import tempfile
from wasm import serve, ProbeHandler
from wasm_features import verify
from wasm_sql import startup
from wasm_toolchain import (WasmError, browser_environment, configuration, digest,
                            read_json, run, run_id, sdk_tools, write_json)


class HistoryHandler(ProbeHandler):
    def send_header(self, keyword, value):
        # Permit history caching of documents without changing the verified
        # asset request/cache policy used by the SDK or other test servers.
        if keyword.lower() == 'cache-control' and self.path.split('?')[0] in ('/multitab.html', '/multitab-empty.html'):
            value = 'no-cache'
        super().send_header(keyword, value)


def check(config, longjmp, engines, selected=None):
    if not engines or len(set(engines)) != len(engines) or any(e not in ('chromium', 'firefox', 'webkit') for e in engines):
        raise WasmError('Invalid multitab browser selection')
    current = config.build / 'wasm/multitab' / run_id(); current.mkdir(parents=True)
    report = {'kind': 'postgamma.wasm-multitab-matrix', 'status': 'failed', 'mode': config.mode,
              'caseSelection': selected or 'all', 'engines': {}, 'runnerSources': {}}
    for name in ('buildsys/wasm_multitab.py', 'tests/wasm/storage/multitab.mjs'):
        report['runnerSources'][name] = digest(config.root / name)
    try:
        receipt = startup(config, longjmp, 'build', 'multitab')
        report['buildReceipt'] = str(receipt)
        site = verify(config, receipt)
        tools, env = sdk_tools(config)
        with serve(site, 'multitab.html', handler=HistoryHandler) as url:
            for engine in engines:
                case = current / engine; case.mkdir()
                command = [tools['node'], str(config.root / 'tests/wasm/storage/multitab.mjs'),
                    '--root', str(config.root), '--engine', engine, '--url', url, '--output', str(case / 'browser.json')]
                if selected: command += ['--case', selected]
                launcher = config.deps / 'browser-libs/launch-webkit.sh'
                if engine == 'webkit' and launcher.is_file(): command += ['--executable', str(launcher)]
                elif engine == 'chromium' and config.browser_override: command += ['--executable', config.browser_override]
                print('wasm multitab: ' + engine, flush=True)
                try:
                    with tempfile.TemporaryDirectory(prefix='pgm-tabs-') as temporary:
                        run(command + ['--profile', str(Path(temporary) / 'profile')], case / 'browser.log',
                            dict(browser_environment(config, env), TMPDIR=temporary), config.root, timeout=900)
                    browser = read_json(case / 'browser.json')
                    if browser['status'] != 'passed' or len(browser['cases']) != (1 if selected else 9):
                        raise WasmError('Incomplete cross-tab evidence')
                    report['engines'][engine] = {'status': 'passed', 'report': str(case / 'browser.json')}
                except Exception as error:
                    report['engines'][engine] = {'status': 'failed', 'report': str(case / 'browser.json'), 'error': str(error)}
                write_json(current / 'report.json', report)
        verify(config, receipt)
        for name, checksum in report['runnerSources'].items():
            if digest(config.root / name) != checksum: raise WasmError('Cross-tab runner changed during validation')
        if any(row['status'] != 'passed' for row in report['engines'].values()):
            raise WasmError('Cross-tab browser matrix failed')
        report['status'] = 'passed'
    except Exception as error:
        report['error'] = str(error); raise
    finally:
        write_json(current / 'report.json', report)
        write_json(config.build / 'wasm/reports/multitab.json', {'status': report['status'], 'run_report': str(current / 'report.json')})
        print(f"wasm multitab: {report['status']}; {current / 'report.json'}", flush=True)
    return current / 'report.json'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, required=True)
    for name in ('build-dir', 'deps-dir', 'sdk-dir', 'profile'): parser.add_argument('--' + name, type=Path)
    parser.add_argument('--target', default='wasm32-emscripten-pthreads')
    parser.add_argument('--mode', default='debug', choices=('debug', 'release'))
    parser.add_argument('--node', default=''); parser.add_argument('--browser-executable', default='')
    parser.add_argument('--longjmp', default='emscripten'); parser.add_argument('--jobs', type=int, default=1)
    parser.add_argument('--engines', default='chromium,firefox,webkit'); parser.add_argument('--case')
    args = parser.parse_args()
    try: check(configuration(args), args.longjmp, args.engines.split(','), args.case)
    except Exception as error: parser.exit(2, 'wasm multitab: ' + str(error).splitlines()[0] + '\n')


if __name__ == '__main__': main()
