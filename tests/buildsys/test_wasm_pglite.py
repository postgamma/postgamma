"""Imported tests must not pass with missing cases, stale provenance or leaked resources."""
import copy
import json
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'buildsys'))
from wasm_pglite import completed_attempt, inventory, validate_case
from wasm_toolchain import WasmError


class PGliteEvidenceTests(unittest.TestCase):
    def row(self):
        cases = [dict(id=name, status='passed') for name in ('first', 'last')]
        return dict(status='passed', cases=copy.deepcopy(cases), pageErrors=[], failedRequests=[], probe=dict(
            status='passed', storage='opfs', cases=cases, isolated=True, worker=True, sharedMemory=True,
            metrics=[dict(resources=dict(locks=0, shmem=0, dsm=0, pipes=0)) for _ in range(2)]))

    def test_complete_evidence_passes(self):
        validate_case(self.row(), ['first', 'last'], 'opfs')

    def test_stalled_case_is_failed_once_before_continuing(self):
        row = dict(cases=[dict(id='first', status='passed')], error='case timed out',
                   events=[dict(kind='case-start', id='first'), dict(kind='case-start', id='stalled')])
        progress = completed_attempt(row, ['first', 'stalled', 'remaining'])
        self.assertEqual([case['id'] for case in progress], ['first', 'stalled'])
        self.assertEqual(progress[-1]['status'], 'failed')
        self.assertFalse(progress[-1]['completed'])
        self.assertEqual(completed_attempt({'error': 'startup failed'}, ['first']), [])
        with self.assertRaises(WasmError): completed_attempt(row, ['different', 'stalled'])

    def test_missing_duplicate_reordered_or_failed_case_rejects(self):
        for target in ('cases', 'probe'):
            for change in ('missing', 'duplicate', 'reordered', 'failed'):
                with self.subTest(target=target, change=change):
                    row = self.row()
                    cases = row['cases'] if target == 'cases' else row['probe']['cases']
                    if change == 'missing': cases.pop()
                    if change == 'duplicate': cases[1] = copy.deepcopy(cases[0])
                    if change == 'reordered': cases.reverse()
                    if change == 'failed': cases[-1]['status'] = 'failed'
                    with self.assertRaises(WasmError): validate_case(row, ['first', 'last'], 'opfs')

    def test_wrong_profile_errors_and_native_leaks_reject(self):
        for problem in ('storage', 'page', 'asset', 'resources', 'missing-close', 'isolation'):
            with self.subTest(problem=problem):
                row = self.row()
                if problem == 'storage': row['probe']['storage'] = 'ephemeral'
                if problem == 'page': row['pageErrors'].append('unexpected trap')
                if problem == 'asset': row['failedRequests'].append('missing wasm')
                if problem == 'resources': row['probe']['metrics'][1]['resources']['dsm'] = 1
                if problem == 'missing-close': row['probe']['metrics'].pop()
                if problem == 'isolation': row['probe']['isolated'] = False
                with self.assertRaises(WasmError): validate_case(row, ['first', 'last'], 'opfs')

    def test_every_selected_case_has_pinned_provenance(self):
        ids, upstream = inventory(ROOT)
        self.assertGreater(len(ids), 0)
        self.assertEqual(len(upstream['commit']), 40)
        for row in upstream['files']:
            self.assertEqual(len(row['sha256']), 64)
            self.assertTrue(row['reason'])
            if row['status'] == 'not-applicable': self.assertEqual(row['cases'], [])

    def test_removed_provenance_cannot_silently_reduce_coverage(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary); directory = root / 'tests/wasm/pglite'; directory.mkdir(parents=True)
            for name in ('cases.json', 'upstream.json'):
                (directory / name).write_bytes((ROOT / 'tests/wasm/pglite' / name).read_bytes())
            upstream = json.loads((directory / 'upstream.json').read_text())
            next(row for row in upstream['files'] if row['cases'])['cases'].pop()
            (directory / 'upstream.json').write_text(json.dumps(upstream))
            with self.assertRaisesRegex(WasmError, 'provenance'): inventory(root)


if __name__ == '__main__': unittest.main()
