"""Reject extension resource comparisons against stale core builds."""
import copy
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'buildsys'))
from wasm_toolchain import WasmError, digest, write_json
from wasm_vector_budget import baseline


class VectorBudgetTests(unittest.TestCase):
    def test_baseline_requires_matching_assets_core_policy_sdk_and_bridge(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            site = directory / 'link/site'
            site.mkdir(parents=True)
            names = ('postgamma.wasm', 'postgamma.data', 'postgamma.mjs')
            for name in names:
                (site / name).write_text(name)
            snapshot = {name: name for name in ('runtime', 'embedded-c', 'manifests')}
            inputs = {'postgres_commit': 'pinned', 'tools': {'compiler': 'pinned-sdk'}}
            bridge = {'sha256': 'bridge', 'flags': ['-DPOSTGAMMA_WASM_TESTING']}
            builder = SimpleNamespace(directory=directory, config=SimpleNamespace(mode='debug'),
                                      source_snapshot=snapshot, inputs=inputs, report={'bridge': bridge})
            run = directory / 'runs/link-1'
            run.mkdir(parents=True)
            report = run / 'report.json'
            link = {'status': 'passed', 'source_snapshot': snapshot, 'inputs': inputs,
                    'bridge': bridge, 'outputs': {name: digest(site / name) for name in names},
                    'audit': {'layout': {}}}
            write_json(report, link)
            write_json(run / 'sql.json', {'status': 'passed', 'scenario': 'all', 'link_report': str(report),
                       'browsers': {'abi': {'probe': {'metrics': [{'peakMemory': 128}]}}}})
            self.assertEqual(baseline(builder)['abi_peak_linear_memory'], 128)
            mutations = [('source_snapshot', 'runtime'), ('source_snapshot', 'manifests'),
                         ('inputs', 'tools'), ('bridge', 'sha256'), ('bridge', 'flags'),
                         ('outputs', 'postgamma.wasm')]
            for field, key in mutations:
                with self.subTest(field=field, key=key):
                    stale = copy.deepcopy(link)
                    stale[field][key] = 'changed'
                    write_json(report, stale)
                    with self.assertRaisesRegex(WasmError, 'matching base artifacts'):
                        baseline(builder)


if __name__ == '__main__':
    unittest.main()
