"""Use the pinned native pg_dump/pg_restore as an independent archive oracle."""
from contextlib import contextmanager
import base64
import json
import os
from pathlib import Path
import pwd
import subprocess
import tempfile
from wasm_toolchain import WasmError, digest, write_json


@contextmanager
def native_oracle(config, directory, site):
    reference = config.build / 'wasm/oracle/reference'
    if not (reference / '.postgamma-configure.json').is_file(): reference = config.build / 'reference'
    metadata = json.loads((reference / '.postgamma-configure.json').read_text())
    if metadata['configure_sha256'] != digest(config.root / 'postgres/configure') or Path(metadata['source']).resolve() != (config.root / 'postgres').resolve():
        raise WasmError('Logical oracle must match the pinned PostgreSQL source')
    prefix = reference / 'tmp_install/usr/local/pgsql'
    binaries = {name: prefix / 'bin' / name for name in ('postgres', 'initdb', 'pg_ctl', 'psql', 'pg_dump', 'pg_restore')}
    for name, binary in binaries.items():
        subdir = 'pg_dump' if name == 'pg_restore' else name
        built = reference / 'src' / ('backend/postgres' if name == 'postgres' else 'bin/' + subdir + '/' + name)
        if not binary.is_file() or not built.is_file() or digest(binary) != digest(built):
            raise WasmError('Build and install the pinned native logical oracle first: ' + name)
    identity = pwd.getpwnam('nobody') if os.geteuid() == 0 else None
    credentials = dict(user=identity.pw_uid, group=identity.pw_gid, extra_groups=()) if identity else {}
    env = dict(os.environ, LD_LIBRARY_PATH=str(prefix / 'lib'), LC_ALL='C')
    log = directory / 'native-oracle.log'
    report = {'status': 'failed', 'binaries': {name: digest(binary) for name, binary in binaries.items()}, 'engines': {}}
    fixture = config.root / 'tests/wasm/sql/logical-interop.sql'
    report['fixtureSha256'] = digest(fixture)

    def call(name, *arguments, data=None):
        result = subprocess.run([str(binaries[name]), *arguments], input=data, capture_output=True,
                                env=env, timeout=45, **credentials)
        with log.open('ab') as output: output.write(result.stderr)
        if result.returncode: raise WasmError('Native logical oracle ' + name + ' failed; see ' + str(log))
        return result.stdout

    with tempfile.TemporaryDirectory(prefix='pgw-logical-') as temporary:
        path = Path(temporary)
        if identity: os.chown(path, identity.pw_uid, identity.pw_gid)
        data, server_log = path / 'data', path / 'postgres.log'
        running = False
        try:
            call('initdb', '-D', str(data), '-A', 'trust', '-U', 'postgamma', '--no-locale', '--encoding=UTF8')
            call('pg_ctl', '-D', str(data), '-l', str(server_log), '-o',
                 f"-k {path} -c listen_addresses='' -c shared_buffers=24MB", '-w', 'start')
            running = True
            connect = ['-h', str(path), '-U', 'postgamma', '-d', 'postgres']

            def sql(query):
                return call('psql', *connect, '-XAt', '-v', 'ON_ERROR_STOP=1', '-c', query).decode().strip()

            sql(fixture.read_text())
            archive = site / 'logical-native.dump'
            archive.write_bytes(call('pg_dump', *connect, '--format=custom', '--compress=none'))
            report['nativeDumpSha256'] = digest(archive)
            query = "SELECT json_agg(v) FROM (SELECT id::text, exact::text, encode(bytes,'hex'), value, missing, phase::text FROM interop.view ORDER BY id) v"
            expected = json.loads(sql(query))

            def validate(engine, browser):
                events = [event for event in browser.get('events', []) if event.get('kind') == 'logical-native-interop']
                if len(events) != 1: raise WasmError('Missing Wasm archive for native pg_restore')
                content = base64.b64decode(events[0]['archive'], validate=True)
                if not content.startswith(b'PGDMP') or not 5 < len(content) < 256 * 1024:
                    raise WasmError('Invalid Wasm interoperability archive')
                output = directory / (engine + '-interop.dump'); output.write_bytes(content)
                sql('DROP SCHEMA interop CASCADE')
                call('pg_restore', *connect, '--exit-on-error', '--single-transaction', data=content)
                if json.loads(sql(query)) != expected or sql('SELECT last_value FROM interop.sequence') != '83' or sql('SELECT interop.plus_one(8)') != '9':
                    raise WasmError('Native restore values differ from Wasm dump')
                if digest(archive) != report['nativeDumpSha256'] or digest(fixture) != report['fixtureSha256']:
                    raise WasmError('Logical interoperability inputs changed')
                report['engines'][engine] = {'status': 'passed', 'wasmDumpSha256': digest(output)}

            yield validate
            report['status'] = 'passed'
        finally:
            try:
                if running: call('pg_ctl', '-D', str(data), '-m', 'immediate', '-w', 'stop')
            finally:
                if server_log.is_file(): (directory / 'native-postgres.log').write_bytes(server_log.read_bytes())
                write_json(directory / 'native-oracle.json', report)
