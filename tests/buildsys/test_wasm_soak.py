"""A partial or short endurance run cannot satisfy a requested long gate."""
import copy
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'buildsys'))
from wasm_soak import CHECKS, validate
from wasm_toolchain import WasmError


class SoakReceiptTests(unittest.TestCase):
    def setUp(self):
        self.parameters = dict(seconds=7200, seed=20261003, rounds=0, reopenEvery=50)
        self.row = dict(status='passed', probe=dict(checks=list(CHECKS)), parameters=self.parameters.copy(),
                        complete=dict(iteration=100, elapsedMs=7200000, crashes=2, orderly=1, cancellations=10),
                        resourceGate=dict(status='passed'))

    def test_long_run_requires_duration_workload_and_resource_evidence(self):
        validate(self.row, self.parameters)
        for field, value in (('elapsedMs', 7199999), ('crashes', 0), ('orderly', 0), ('cancellations', 0)):
            row = copy.deepcopy(self.row); row['complete'][field] = value
            with self.subTest(field=field), self.assertRaises(WasmError): validate(row, self.parameters)
        row = copy.deepcopy(self.row); row['resourceGate']['status'] = 'not-run'
        with self.assertRaises(WasmError): validate(row, self.parameters)
        row = copy.deepcopy(self.row); row['probe']['checks'].pop()
        with self.assertRaises(WasmError): validate(row, self.parameters)

    def test_seed_and_exact_replay_rounds_are_required(self):
        row = copy.deepcopy(self.row); row['parameters']['seed'] += 1
        with self.assertRaises(WasmError): validate(row, self.parameters)
        self.parameters.update(seconds=120, rounds=100)
        self.row['parameters'] = self.parameters.copy()
        self.row['resourceGate']['status'] = 'not-run'; self.row['complete']['elapsedMs'] = 10
        validate(self.row, self.parameters)
        self.row['complete']['iteration'] -= 1
        with self.assertRaises(WasmError): validate(self.row, self.parameters)

    def test_profiling_receipt_cannot_qualify_even_with_passed_fields(self):
        self.row['kind'] = 'postgamma.wasm-memory-diagnostic'
        with self.assertRaises(WasmError): validate(self.row, self.parameters)


if __name__ == '__main__': unittest.main()
