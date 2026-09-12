"""Run the shared SQL/COPY fixture against the pinned native PostgreSQL build."""
from __future__ import annotations
import json
import os
import re
from pathlib import Path
import subprocess
import tempfile
from wasm_toolchain import WasmError, digest, write_json


def oracle(root: Path, directory: Path, site: Path, build: Path | None = None) -> dict:
    build = build or root/'build'
    reference = build/'wasm/oracle/reference'
    if not (reference/'.postgamma-configure.json').is_file():
        reference = build/'reference'
    prefix = reference/'tmp_install/usr/local/pgsql'
    source_version = re.search(r'AC_INIT\(\[PostgreSQL\], \[([^]]+)\]', (root/'postgres/configure.ac').read_text())[1]
    configuration = json.loads((reference/'.postgamma-configure.json').read_text())
    if configuration['configure_sha256'] != digest(root/'postgres/configure') or Path(configuration['source']).resolve() != (root/'postgres').resolve():
        raise WasmError('native oracle configuration does not match the target source')
    binaries = {name: prefix / 'bin' / name for name in ('postgres', 'initdb', 'pg_ctl', 'psql')}
    if not all(p.is_file() for p in binaries.values()):
        raise WasmError('native SQL oracle is unavailable; build the reference PostgreSQL test installation first')
    for name, binary in binaries.items():
        source = reference/'src'/('backend/postgres' if name == 'postgres' else 'bin/'+name+'/'+name)
        if not source.is_file() or digest(binary) != digest(source):
            raise WasmError('native oracle installation differs from its current build: '+name)
    fixture_path = root / 'tests/wasm/sql/fixtures.json'
    fixture = json.loads(fixture_path.read_text())
    env = dict(os.environ, LD_LIBRARY_PATH=str(prefix / 'lib'), LC_ALL='C')
    log = directory / 'oracle.log'
    def call(name, *args, input=None):
        result = subprocess.run([str(binaries[name]), *args], env=env, text=True,
                                input=input, capture_output=True, timeout=30)
        with log.open('a') as output:
            output.write(result.stdout + result.stderr)
        if result.returncode:
            raise WasmError(f'native SQL oracle {name} failed; see {log}')
        return result.stdout.strip()
    with tempfile.TemporaryDirectory(prefix='pgw-') as temporary:
        path = Path(temporary)
        data = path / 'data'
        call('initdb', '-D', str(data), '-A', 'trust', '-U', 'postgamma', '--no-locale')
        running = False
        try:
            call('pg_ctl', '-D', str(data), '-l', str(directory / 'oracle-postgres.log'),
                 '-o', f"-k {path} -c listen_addresses='' -c shared_buffers=24MB", '-w', 'start')
            running = True
            def sql(query, input=None):
                return call('psql', '-XAt', '-v', 'ON_ERROR_STOP=1', '-h', str(path),
                            '-U', 'postgamma', '-d', 'postgres', '-c', query, input=input)
            version = sql('SELECT version()')
            if not version.startswith('PostgreSQL ' + source_version + ' '):
                raise WasmError('native oracle version differs from the pinned kernel')
            values = {}
            for name in ('basic', 'binary'):
                values[name] = json.loads(sql('SELECT json_agg(value::text ORDER BY value) FROM (' +
                                            fixture[name] + ') AS q(value)'))
            sql(fixture['copyCreate'])
            copy_input = '1,alpha\n2,' + 'x' * fixture['copyPayloadBytes'] + '\n3,omega\n'
            sql(fixture['copyIn'], copy_input)
            copy_output = sql(fixture['copyOut']) + '\n'
            if copy_output != copy_input:
                raise WasmError('native COPY roundtrip changed the fixture bytes')
            values['copy'] = copy_output
            report = {'kind': 'postgamma.wasm-sql-oracle', 'status': 'passed', 'version': version,
                      'source_commit': subprocess.check_output(['git','-C',str(root/'postgres'),'rev-parse','HEAD'],text=True).strip(),
                      'source_version':source_version,
                      'binaries': {name: digest(p) for name, p in binaries.items()},
                      'inputs': {str(fixture_path.relative_to(root)): digest(fixture_path),
                                 'tests/embedded/api/chunked_results_driver.c': digest(root / 'tests/embedded/api/chunked_results_driver.c'),
                                 'tests/embedded/api/copy_driver.c': digest(root / 'tests/embedded/api/copy_driver.c')},
                      'values': values}
        finally:
            # Only the private cluster started above is stopped or removed.
            if running:
                call('pg_ctl', '-D', str(data), '-m', 'immediate', '-w', 'stop')
    write_json(directory / 'oracle.json', report)
    write_json(site / 'oracle.json', values)
    write_json(site / 'fixtures.json', fixture)
    return report
