"""Reject parallel qualification without actual two-worker execution evidence."""
import copy
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'buildsys'))
from wasm_features import parallel_plans
from wasm_toolchain import WasmError


class ParallelEvidenceTests(unittest.TestCase):
    def evidence(self):
        labels = [prefix + '-' + case for prefix in ('ephemeral', 'opfs') for case in
                  ('scan', 'aggregate', 'hash-join', 'gather-merge', 'worker-guc',
                   'snapshot', 'leader-excluded', 'reuse-after-errors')] + ['opfs-reopen']
        return {'events': [{'kind': 'parallel-plan', 'label': label, 'plan': {
            'Node Type': 'Aggregate', 'Plans': [{'Node Type': 'Gather',
                'Workers Planned': 2, 'Workers Launched': 2}]}} for label in labels]}

    def test_accepts_complete_actual_two_worker_plans(self):
        self.assertEqual(parallel_plans(self.evidence()), 17)

    def test_rejects_configured_or_planned_but_not_launched_workers(self):
        for launched in (None, 0, 1):
            row = self.evidence()
            row['events'][0]['plan']['Plans'][0]['Workers Launched'] = launched
            with self.subTest(launched=launched), self.assertRaisesRegex(WasmError, 'two launched'):
                parallel_plans(row)

    def test_rejects_serial_plans_and_missing_persistent_reopen(self):
        row = self.evidence()
        serial = copy.deepcopy(row)
        serial['events'][0]['plan'] = {'Node Type': 'Seq Scan'}
        with self.assertRaisesRegex(WasmError, 'two launched'):
            parallel_plans(serial)
        row['events'].pop()
        with self.assertRaisesRegex(WasmError, 'Missing'):
            parallel_plans(row)


if __name__ == '__main__':
    unittest.main()
