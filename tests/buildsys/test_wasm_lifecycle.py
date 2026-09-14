"""Repeated recovery evidence must not pass with missing or stale observations."""
import copy
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'buildsys'))
from wasm_lifecycle import validate_case, verify_inputs
from wasm_toolchain import WasmError, digest


class LifecycleEvidenceTests(unittest.TestCase):
    def test_all_iterations_and_recovery_bounds_are_required(self):
        row = {'status': 'passed', 'probe': {'status': 'passed'}, 'events': [
            event for iteration in range(1, 21) for event in (
                {'kind': 'fatal-iteration', 'fault': 1, 'iteration': iteration},
                {'kind': 'recovery-timing', 'elapsedMs': 1000})]}
        validate_case(row, 'fatal-opfs-trap', 20)
        for mutation in ('missing', 'duplicate', 'wrong-fault', 'timeout'):
            with self.subTest(mutation=mutation):
                broken = copy.deepcopy(row)
                if mutation == 'missing': broken['events'].pop()
                if mutation == 'duplicate': broken['events'][-2]['iteration'] = 19
                if mutation == 'wrong-fault': broken['events'][-2]['fault'] = 2
                if mutation == 'timeout': broken['events'][-1]['elapsedMs'] = 30000
                with self.assertRaises(WasmError):
                    validate_case(broken, 'fatal-opfs-trap', 20)

    def test_reused_receipt_rejects_changed_sources_and_assets(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'source.ts').write_text('original')
            (root / 'module.js').write_text('original')
            (root / 'postgamma.assets.json').write_text('{"mode":"release"}')
            sql = {'status': 'passed', 'action': 'check', 'sql_validated': True, 'site': str(root),
                   'harness': {'module.js': digest(root / 'module.js')},
                   'harness_sources': {'source.ts': digest(root / 'source.ts')}}
            config = SimpleNamespace(root=root, mode='release')
            verify_inputs(config, sql)
            for name in ('source.ts', 'module.js'):
                (root / name).write_text('changed')
                with self.assertRaisesRegex(WasmError, 'changed'):
                    verify_inputs(config, sql)
                (root / name).write_text('original')
            with self.assertRaisesRegex(WasmError, 'mode differs'):
                verify_inputs(SimpleNamespace(root=root, mode='debug'), sql)
            sql['action'] = 'build'
            with self.assertRaisesRegex(WasmError, 'passing SQL check'):
                verify_inputs(config, sql)


if __name__ == '__main__':
    unittest.main()
