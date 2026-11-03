#!/usr/bin/env python3
"""Exercise the browser demos with real Wasm, including a headerless Pages host."""
from __future__ import annotations
import argparse
from http.server import SimpleHTTPRequestHandler
from pathlib import Path

from wasm import serve
from wasm_toolchain import browser_environment, configuration, digest, read_json, run, run_id, sdk_tools, write_json


class PagesHandler(SimpleHTTPRequestHandler):
    def translate_path(self, path):
        # Project Pages sites live below a repository prefix.
        if path.startswith('/project/'):
            path = path[len('/project'):]
        return super().translate_path(path)

    def log_message(self, *args):
        pass


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, required=True)
    parser.add_argument('--build-dir', type=Path)
    parser.add_argument('--site-dir', type=Path)
    parser.add_argument('--engines', default='chromium,firefox,webkit')
    parser.add_argument('--clients', default='psql,js,opfs', help='comma-separated psql, js, and/or opfs')
    parser.add_argument('--hosts', default='headers,pages', help='comma-separated headers and/or pages')
    parser.set_defaults(deps_dir=None, profile=None, sdk_dir=None, jobs=1,
                        target='wasm32-emscripten-pthreads', mode='release', node='', browser_executable='')
    args = parser.parse_args()
    config = configuration(args)
    tools, env = sdk_tools(config)
    site = args.site_dir or config.build / 'docs/site'
    clients = args.clients.split(',')
    if not clients or any(client not in {'psql', 'js', 'opfs'} for client in clients):
        parser.error('clients must be selected from psql, js, opfs')
    hosts = args.hosts.split(',')
    if not hosts or any(host not in {'headers', 'pages'} for host in hosts):
        parser.error('hosts must be selected from headers, pages')
    harnesses = {client: Path(__file__).resolve().parents[1] / 'tests/wasm/docs' /
                 {'psql': 'psql.mjs', 'js': 'homepage.mjs', 'opfs': 'opfs.mjs'}[client] for client in clients}
    assets = ['index.html', 'live-worker.js', 'assets/javascripts/live.js',
              'assets/javascripts/psql.js', 'assets/javascripts/psql-terminal.js', 'assets/javascripts/psql-examples.js', 'assets/javascripts/live-report.js',
              'assets/vendor/xterm/xterm.mjs', 'assets/vendor/xterm/addon-fit.mjs',
              'assets/vendor/xterm/xterm.css',
              'assets/stylesheets/live.css', 'assets/stylesheets/extra.css',
              'assets/vendor/fonts/fonts.css',
              'assets/wasm/postgamma.assets.json']
    assets += [path.relative_to(site).as_posix() for path in sorted((site / 'assets/vendor/fonts').glob('*.woff2'))]
    if 'opfs' in clients:
        assets += ['opfs/index.html', 'assets/javascripts/opfs.js', 'assets/stylesheets/opfs.css']
    identity = {name: digest(site / name) for name in assets}
    current = config.build / 'docs/checks' / run_id()
    current.mkdir(parents=True)
    report = {'status': 'passed', 'results': [], 'site_files': identity,
              'harnesses': {client: digest(harness) for client, harness in harnesses.items()},
              'package': read_json(site / 'assets/wasm/demo.json')}
    for engine in args.engines.split(','):
        if engine not in {'chromium', 'firefox', 'webkit'}:
            parser.error('unknown browser engine: ' + engine)
        for client, static in [(client, host == 'pages') for client in clients for host in hosts]:
            harness = harnesses[client]
            name = engine + '-' + client + ('-pages' if static else '-headers')
            output = current / (name + '.json')
            kwargs = {'handler': PagesHandler} if static else {}
            with serve(site, 'project/' if static else '', **kwargs) as url:
                command = [tools['node'], str(harness),
                           '--root', str(config.root), '--url', url, '--engine', engine, '--output', str(output)]
                if static:
                    command.append('--static-host')
                launcher = config.deps / 'browser-libs/launch-webkit.sh'
                if engine == 'webkit' and launcher.is_file():
                    command += ['--executable', str(launcher)]
                elif engine == 'chromium' and config.browser_override:
                    command += ['--executable', config.browser_override]
                try:
                    run(command, current / (name + '.log'), browser_environment(config, env), config.root,
                        timeout=360 if client == 'opfs' else 180)
                    report['results'].append({'name': name, 'status': 'passed', 'report': str(output)})
                except Exception as error:
                    report['status'] = 'failed'
                    report['results'].append({'name': name, 'status': 'failed', 'report': str(output), 'error': str(error)})
            print(name + ': ' + report['results'][-1]['status'], flush=True)
    if (identity != {name: digest(site / name) for name in assets} or
            report['harnesses'] != {client: digest(harness) for client, harness in harnesses.items()}):
        report['status'] = 'failed'
        report['error'] = 'Demo or test source changed during validation'
    write_json(current / 'result.json', report)
    write_json(config.build / 'docs/live-check.json', {'status': report['status'], 'run_report': str(current / 'result.json')})
    print('documentation live check: ' + str(current / 'result.json'), flush=True)
    if report['status'] != 'passed':
        raise SystemExit(1)


if __name__ == '__main__':
    main()
