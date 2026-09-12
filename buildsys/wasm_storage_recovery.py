"""Run real OPFS crash boundaries in one origin and persistent browser profile."""
from pathlib import Path
import tempfile
from wasm_toolchain import WasmError, browser_environment, read_json, run


def check(config, builder, url: str) -> dict:
    output = builder.current / 'cases/recovery'
    output.mkdir(parents=True, exist_ok=True)
    command = [builder.tools['node'], str(config.root / 'tests/wasm/storage/recovery.mjs'),
               '--root', str(config.root), '--url', url, '--output', str(output / 'browser.json'),
               '--profile', str(output / 'profile')]
    if config.browser_override:
        command += ['--executable', str(Path(config.browser_override).resolve())]
    with tempfile.TemporaryDirectory(prefix='pgm-wasm-', dir='/tmp') as temporary:
        run(command, output / 'runner.log', dict(browser_environment(config, builder.env), TMPDIR=temporary),
            config.root, timeout=600)
    report = read_json(output / 'browser.json')
    if report.get('status') != 'passed' or len(report.get('cases', [])) != 18:
        raise WasmError('incomplete storage recovery matrix')
    if any(case.get('status') != 'passed' for case in report['cases']):
        raise WasmError('storage recovery case failed')
    return report
