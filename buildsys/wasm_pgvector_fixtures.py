"""Adapt the existing native and pinned upstream pgvector SQL fixtures."""
import ast
import re
from pathlib import Path
from wasm_toolchain import WasmError, digest, write_json


def l2_statements(sql, expected):
    sql = sql.split('-- inner product', 1)[0]
    expected = expected.split('-- inner product', 1)[0]
    statements = [s.strip() + ';' for s in re.sub(r'^\s*--[^\n]*', '', sql, flags=re.M).split(';') if s.strip()]
    result, cursor = [], 0
    for sql in statements:
        offset = expected.find(sql, cursor)
        if offset < 0:
            raise WasmError('upstream expected output is missing fixture SQL')
        cursor = offset + len(sql)
        entry = {'sql': sql}
        if sql.startswith('SELECT'):
            output = expected[cursor:].lstrip('\n')
            match = re.match(r'[^\n]*\n[-+]+\n(.*?)\((\d+) rows?\)\n', output, re.S)
            if not match:
                raise WasmError('upstream result table is not recognized')
            entry['rows'] = [[cell.strip() for cell in line.split('|')] for line in match[1].splitlines()]
            if len(entry['rows']) != int(match[2]):
                raise WasmError('upstream result row count differs from its table')
        result.append(entry)
    if len(result) != 11 or sum('rows' in entry for entry in result) != 4:
        raise WasmError('upstream L2 fixture scope changed')
    return result


def fixtures(root, directory, site):
    native = root / 'tests/embedded/release/multi_instance_driver.c'
    groups = re.findall(r'(?:"(?:\\.|[^"\\])*"\s*)+', native.read_text())
    strings = [''.join(ast.literal_eval(s) for s in re.findall(r'"(?:\\.|[^"\\])*"', group)) for group in groups]
    starts = ['CREATE TABLE postgamma_release_vector_hnsw ', 'INSERT INTO postgamma_release_vector_hnsw ',
              'CREATE TABLE postgamma_release_vector_ivf ', 'CREATE INDEX postgamma_release_vector_hnsw_idx ',
              'CREATE INDEX postgamma_release_vector_ivf_idx ', 'ANALYZE postgamma_release_vector_hnsw',
              'ANALYZE postgamma_release_vector_ivf']
    setup = []
    for prefix in starts:
        matches = [sql for sql in strings if sql.startswith(prefix)]
        if len(matches) != 1:
            raise WasmError('native pgvector fixture changed: ' + prefix)
        setup.append(matches[0])
    result = {'nativeSetup': setup, 'upstream': {}}
    inputs = {str(native.relative_to(root)): digest(native)}
    for index in ('hnsw', 'ivfflat'):
        source = root / ('third_party/pgvector/test/sql/' + index + '_vector.sql')
        expected = root / ('third_party/pgvector/test/expected/' + index + '_vector.out')
        result['upstream'][index] = l2_statements(source.read_text(), expected.read_text())
        inputs.update({str(p.relative_to(root)): digest(p) for p in (source, expected)})
    write_json(site / 'vector-fixtures.json', result)
    report = {'kind': 'postgamma.wasm-vector-fixtures', 'status': 'passed', 'inputs': inputs,
              'output_sha256': digest(site / 'vector-fixtures.json'),
              'scope': 'native 2000-row dataset and index definitions; upstream L2, NULL, insert and truncate cases'}
    write_json(directory / 'vector-fixtures.json', report)
    return report
