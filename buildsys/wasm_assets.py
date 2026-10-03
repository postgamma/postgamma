"""Bind the browser API, bridge, kernel and resources to one build identity."""
import json
import re
from product_version import read_version
from wasm_toolchain import digest, fingerprint, read_json, write_json


def npm_version(root):
    version = read_version(root)
    result = f'{version.major}.{version.minor}.{version.patch}'
    if version.phase:
        result += '-' + {'a': 'alpha', 'b': 'beta', 'rc': 'rc'}[version.phase] + '.' + str(version.serial)
    return result


def generate(builder, worker='src/worker/sql.js'):
    site, root = builder.output / 'site', builder.root
    files = [*sorted((site / 'src').rglob('*.js')), *sorted(site.glob('postgamma.*'))]
    files = [p for p in files if p.name not in ('identity.js', 'postgamma.assets.json')]
    hashes = {str(p.relative_to(site)): {'sha256': digest(p), 'bytes': p.stat().st_size} for p in files}
    wire = (root / 'wasm/src/protocol/wire.ts').read_text()
    protocol = int(re.search(r'export const VERSION = (\d+);', wire)[1])
    policy = read_json(root / 'manifests/profiles/wasm-resource-budget.json')
    manifest = {'schemaVersion': 1, 'version': npm_version(root), 'productVersion': read_version(root).text,
                'target': builder.config.target, 'mode': builder.config.mode, 'protocolVersion': protocol,
                'postgresCommit': builder.inputs['postgres_commit'],
                'extensions': {name: (read_json(root / 'manifests/extensions/pgvector-upstream.json')['commit']
                    if name == 'pgvector' else builder.inputs['postgres_commit']) for name in builder.extensions},
                'bridge': {'sha256': builder.report['bridge']['sha256'], 'testing': bool(builder.report['bridge']['flags'])},
                'entries': {'worker': worker, 'module': 'postgamma.mjs', 'wasm': 'postgamma.wasm', 'resources': 'postgamma.data'},
                'capabilities': {'sql': worker.endswith('/sql.js'), 'independentSessions': 4,
                                 'storage': ['ephemeral-wasmfs', 'owned-opfs'], 'arrowRuntime': False,
                                 'maximumMemoryBytes': policy['maximum_memory_bytes'],
                                 'requirements': ['crossOriginIsolated', 'SharedArrayBuffer', 'WebAssembly', 'moduleWorker', 'WebCrypto'],
                                 'opfsRequirements': ['WebLocks', 'FileSystemSyncAccessHandle', 'BroadcastChannel']},
                'files': hashes}
    manifest['buildId'] = fingerprint(manifest)
    identity = site / 'src/loader/identity.js'
    identity.write_text('export const buildId = ' + json.dumps(manifest['buildId']) + ';\nexport const version = ' + json.dumps(manifest['version']) + ';\n')
    hashes['src/loader/identity.js'] = {'sha256': digest(identity), 'bytes': identity.stat().st_size}
    write_json(site / 'postgamma.assets.json', manifest)
    return manifest
