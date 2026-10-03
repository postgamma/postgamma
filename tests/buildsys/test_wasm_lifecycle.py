"""Repeated recovery evidence must not pass with missing or stale observations."""
import copy
import json
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'buildsys'))
from wasm_lifecycle import validate_case, verify_inputs
from wasm_toolchain import WasmError, digest
from wasm_link import source_snapshot


class LifecycleEvidenceTests(unittest.TestCase):
    def test_diagnostic_requires_waiter_owner_and_unchanged_command(self):
        snapshot = {'driver': {'testHold': 1, 'state': 1, 'phase': 3, 'sequence': 5, 'thread': 16},
                    'currentCommand': {'op': 65530}, 'storage': {'store': {'handles': 2}},
                    'threads': [{'id': 16, 'waitPoint': 'database-notification'}]}
        row = {'status': 'passed', 'probe': {'status': 'passed', 'scenario': 'diagnostics',
               'checks': ['timeout-snapshot', 'snapshot-does-not-resume', 'resume-after-snapshot']},
               'diagnosticGate': {phase: [{'snapshot': copy.deepcopy(snapshot)}] for phase in ('before', 'after')}}
        validate_case(row, 'diagnostics', 20)
        for mutation in ('missing', 'resumed', 'sequence', 'waiter', 'owner'):
            broken = copy.deepcopy(row)
            observed = broken['diagnosticGate']['after'][0]['snapshot']
            if mutation == 'missing': del broken['diagnosticGate']
            if mutation == 'resumed': observed['driver']['testHold'] = 0
            if mutation == 'sequence': observed['driver']['sequence'] = 6
            if mutation == 'waiter': observed['threads'] = []
            if mutation == 'owner': observed['storage'] = {}
            with self.subTest(mutation=mutation), self.assertRaises(WasmError):
                validate_case(broken, 'diagnostics', 20)

    def test_control_and_startup_require_each_ownership_observation(self):
        required = {
            'control-progress': ['cancel-yields-to-sql', 'cancel-yields-to-cancel', 'release-yields-to-sql', 'release-yields-to-cancel', 'copy-close-yields-to-sql', 'copy-close-yields-to-cancel',
                'copy-abort-yields-to-sql', 'copy-abort-yields-to-cancel', 'unclaimed-copy-in-yields-to-sql', 'unclaimed-copy-in-yields-to-cancel', 'copy-in-close', 'unclaimed-copy-release'],
            'control-timeout': ['control-bounded-deadline', 'control-timeout-retry', 'control-stale-generation'],
            'connect-progress': ['connect-yields-to-sql', 'connect-yields-to-cancel', 'connect-startup-backpressure', 'connect-failure-retirement',
                'connect-deadline-retirement', 'connect-stale-generation', 'connect-carrier-reservation', 'connect-pinned-commit'],
        }
        for scenario, checks in required.items():
            row = {'status': 'passed', 'probe': {'status': 'passed', 'scenario': scenario, 'checks': checks},
                   'events': [{'kind': 'control-timeout', 'elapsedMs': 10001}]}
            validate_case(row, scenario, 20)
            for check in checks:
                broken = copy.deepcopy(row)
                broken['probe']['checks'].remove(check)
                with self.subTest(scenario=scenario, check=check), self.assertRaises(WasmError):
                    validate_case(broken, scenario, 20)
            if scenario == 'control-timeout':
                for elapsed in (0, 9899, 15000):
                    broken = copy.deepcopy(row)
                    broken['events'][0]['elapsedMs'] = elapsed
                    with self.assertRaises(WasmError):
                        validate_case(broken, scenario, 20)

    def test_connection_progress_requires_all_observations(self):
        row = {'status': 'passed', 'probe': {'status': 'passed', 'scenario': 'lifecycle', 'checks': [
            'disconnect-yields-to-sql', 'disconnect-yields-to-cancel', 'disconnect-waits-for-backend-release']}}
        validate_case(row, 'lifecycle', 20)
        for check in row['probe']['checks']:
            broken = copy.deepcopy(row)
            broken['probe']['checks'].remove(check)
            with self.assertRaisesRegex(WasmError, 'missing connection'):
                validate_case(broken, 'lifecycle', 20)

    def test_connection_deadline_requires_timing_and_retry(self):
        row = {'status': 'passed', 'probe': {'status': 'passed', 'scenario': 'disconnect-timeout', 'checks': [
            'disconnect-bounded-deadline', 'disconnect-timeout-retry', 'disconnect-rejects-new-work',
            'disconnect-stale-generation']}, 'events': [{'kind': 'disconnect-timeout', 'elapsedMs': 10001, 'attempts': 50}]}
        validate_case(row, 'disconnect-timeout', 20)
        for mutation in ('missing', 'duplicate', 'early', 'late', 'no-retry', 'wrong-scenario'):
            broken = copy.deepcopy(row)
            if mutation == 'missing': broken['events'] = []
            if mutation == 'duplicate': broken['events'] *= 2
            if mutation == 'early': broken['events'][0]['elapsedMs'] = 100
            if mutation == 'late': broken['events'][0]['elapsedMs'] = 15000
            if mutation == 'no-retry': broken['events'][0]['attempts'] = 1
            if mutation == 'wrong-scenario': broken['probe']['scenario'] = 'lifecycle'
            with self.subTest(mutation=mutation), self.assertRaises(WasmError):
                validate_case(broken, 'disconnect-timeout', 20)

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
            (root / 'buildsys').mkdir()
            (root / 'runtime').mkdir()
            (root / 'runtime/io.c').write_text('original')
            (root / 'wasm/bridge').mkdir(parents=True)
            (root / 'wasm/bridge/sql.c').write_text('bridge')
            (root / 'tests/wasm/sql').mkdir(parents=True)
            (root / 'tests/wasm/sql/close_gate.c').write_text('native fixture')
            link = {'status': 'passed', 'kernel_linked': True, 'extensions': [],
                    'source_snapshot': source_snapshot(root),
                    'bridge': {'sha256': digest(root / 'wasm/bridge/sql.c')}}
            (root / 'link.json').write_text(json.dumps(link))
            sql = {'status': 'passed', 'action': 'check', 'sql_validated': True, 'site': str(root),
                   'link_report': str(root / 'link.json'),
                   'harness': {'module.js': digest(root / 'module.js')},
                   'harness_sources': {'source.ts': digest(root / 'source.ts')}}
            config = SimpleNamespace(root=root, mode='release')
            verify_inputs(config, sql)
            for name in ('source.ts', 'module.js', 'runtime/io.c', 'wasm/bridge/sql.c', 'tests/wasm/sql/close_gate.c'):
                original = (root / name).read_text()
                (root / name).write_text('changed')
                with self.assertRaisesRegex(WasmError, 'changed'):
                    verify_inputs(config, sql)
                (root / name).write_text(original)
            with self.assertRaisesRegex(WasmError, 'mode differs'):
                verify_inputs(SimpleNamespace(root=root, mode='debug'), sql)
            sql['action'] = 'build'
            with self.assertRaisesRegex(WasmError, 'passing SQL check'):
                verify_inputs(config, sql)


if __name__ == '__main__':
    unittest.main()
