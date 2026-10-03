"""Compile PostgreSQL contrib modules through the reviewed role-state pipeline."""
from pathlib import Path
import re
import shlex
import sys

from filter_compile_database import select_compile_records
from wasm_generate import ast_cached, tree_hashes
from wasm_toolchain import WasmError, capture, digest, read_json, run, write_json

CONTRIB = ('pg_trgm', 'hstore')
STANDARD = ('pgvector', *CONTRIB)


def selection(value):
    if value == 'none': return ()
    names = STANDARD if value == 'standard' else tuple(value.split(','))
    if len(set(names)) != len(names) or any(name not in STANDARD for name in names):
        raise WasmError('Unsupported Wasm extension selection')
    return tuple(name for name in STANDARD if name in names)


def sql_symbols(source):
    # Only LANGUAGE C declarations bind module symbols; SQL operators often
    # use a different SQL function name from their C entry point.
    symbols = set()
    pattern = re.compile(r"CREATE\s+(?:OR\s+REPLACE\s+)?FUNCTION\s+([\w]+)\s*\([^;]*?\bAS\s+'MODULE_PATHNAME'(?:\s*,\s*'([\w]+)')?[^;]*;", re.I | re.S)
    for path in sorted(source.glob('*.sql')):
        for match in pattern.finditer(path.read_text()): symbols.add(match[2] or match[1])
    if not symbols: raise WasmError('Contrib SQL declares no module functions')
    return {'Pg_magic_func', *symbols, *('pg_finfo_' + symbol for symbol in symbols),
            *({'_PG_init'} if source.name == 'pg_trgm' else set())}


def build(builder, module):
    name, root, env = module['id'], builder.root, builder.env
    source = root / 'postgres/contrib' / name
    sources = sorted(source.glob('*.c'))
    symbols = {entry['logical_name']: entry['linker_name'] for entry in module['symbols']}
    if set(symbols) != sql_symbols(source):
        raise WasmError('Contrib static symbol manifest differs from SQL: ' + name)
    version = re.search(r"default_version\s*=\s*'([^']+)'", (source / (name + '.control')).read_text())[1]
    if version != module['version']: raise WasmError('Contrib control version changed: ' + name)
    output = builder.output.parent / name; output.mkdir(parents=True, exist_ok=True)
    current = builder.current / name; current.mkdir()
    database = output / 'compile-db'
    fragments = current / 'fragments'; fragments.mkdir()
    capture_env = dict(env, EM_COMPILER_WRAPPER=str(root / 'buildsys/wasm_cc_capture.py'), POSTGAMMA_CAPTURE_DIR=str(fragments))
    adapter = ['-include', str(root / 'extensions/wasm-contrib/state.h')]
    for path in sources:
        run([builder.emcc, '-std=gnu11', *builder.flags, *adapter, '-I' + str(source),
             '-c', str(path), '-o', str(output / (path.stem + '.o'))], builder.log, capture_env, output)
    rows = [read_json(path) for path in sorted(fragments.glob('*.json'))]
    domain = root / 'manifests/domains' / (name + '.json')
    rows, selected = select_compile_records(rows, source, None, read_json(domain))
    if selected['selected_translation_units'] != len(sources): raise WasmError('Incomplete contrib compilation inventory')
    write_json(database / 'compile_commands.json', rows)
    selected_sources = shlex.split(capture([sys.executable, str(root / 'buildsys/list_compile_sources.py'),
        '--database', str(database / 'compile_commands.json'), '--source-root', str(source),
        '--domain-manifest', str(domain), '--selection-report', str(current / 'selection.json')], env))
    if set(selected_sources) != {str(path) for path in sources}: raise WasmError('Incomplete contrib AST selection')
    from wasm_kernel_checks import validate_database
    capture_report = validate_database(database / 'compile_commands.json', root, builder.config.build,
        Path(builder.tools['settings']['LLVM_ROOT']) / 'clang', Path(builder.tools['settings']['CACHE']) / 'sysroot')
    generation = read_json(Path(builder.report['generation_report']))
    catalog = output / 'state.json'
    identity = {'database': digest(database / 'compile_commands.json'), 'sources': tree_hashes(source),
                'adapter': tree_hashes(root / 'extensions/wasm-contrib'),
                'generation': digest(Path(builder.report['generation_report'])), 'tools': generation['host_tools']}
    ast_cached([generation['host_tools']['postgamma-state-inventory']['path'],
        '--source-root=' + str(source), '--definition-domain=' + str(domain),
        '--clang-resource-dir=' + generation['target_resource_directory'], '--output=' + str(catalog),
        '-p=' + str(database), *(str(path) for path in sources)], [catalog], identity, builder.log, env, root)
    command = [sys.executable, str(root / 'buildsys/generate_extension_state_plan.py')]
    for key, value in {'catalog': catalog, 'policy': root / 'manifests/ownership/extensions' / (name + '.json'),
        'source-root': source, 'selection': current / 'selection.json',
        'backend-state-policy': builder.directory / 'inventory/backend-policy.json',
        'backend-state-runtime': builder.directory / 'generated/runtime/.postgamma-backend-state-runtime.json',
        'guc-manifest': builder.directory / 'inventory/guc-transform.json',
        'output': current / 'plan.json', 'report': current / 'state.json'}.items():
        command += ['--' + key, str(value)]
    run(command, builder.log, env, root)
    generated = output / 'generated'
    run([sys.executable, str(root / 'buildsys/materialize.py'), '--repository', str(root / 'postgres'),
         '--manifest', str(root / 'manifests/upstream.json'), '--destination', str(generated),
         '--build-root', str(builder.config.build), '--subdirectory', 'contrib/' + name], builder.log, env, root)
    run([sys.executable, str(root / 'buildsys/apply_replacements.py'), '--plan', str(current / 'plan.json'),
         '--tree', str(generated)], builder.log, env, root)
    jobs = [(generated / path.name, 'modules/' + name + '/' + path.stem,
             [*adapter, '-I' + str(generated)]) for path in sources]
    builder.namespace_group(name, jobs, mapping=symbols)
    builder.report[name] = {'postgres_commit': builder.inputs['postgres_commit'],
        'version': version, 'capture': capture_report, 'compile_database_sha256': digest(database / 'compile_commands.json'),
        'catalog_sha256': digest(catalog), 'plan_sha256': digest(current / 'plan.json'),
        'state_report': str(current / 'state.json'), 'state_summary': read_json(current / 'state.json')['summary'],
        'generated': tree_hashes(generated), 'registered_symbols': len(symbols)}
