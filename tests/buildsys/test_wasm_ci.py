"""CI scheduling and diagnostics must preserve failures and kernel boundaries."""
from pathlib import Path
import sys
import json
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch
ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT/'buildsys'))
from plan_wasm_ci import plan, shared_input
from collect_wasm_evidence import selected
from wasm_ci import run_suite, stages
from wasm_toolchain import WasmError

class WasmCITests(unittest.TestCase):
    def test_shared_changes_require_native_regression(self):
        for path in ('runtime/platform/emscripten/storage.cpp', 'runtime/src/session.c',
                     'compiler/src/transform.cpp', 'embedded-c/src/api.c',
                     'manifests/thread-state.json', 'buildsys/apply_replacements.py', 'postgres'):
            with self.subTest(path=path): self.assertTrue(shared_input(path))
        for path in ('wasm/src/api/database.ts', 'tests/wasm/sql/results.ts',
                     'buildsys/wasm_link.py', 'manifests/profiles/wasm-kernel.json'):
            with self.subTest(path=path): self.assertFalse(shared_input(path))
        self.assertTrue(plan(ROOT, '')['native'])
        self.assertTrue(plan(ROOT, '0'*40)['native'])
        with self.assertRaises(ValueError): plan(ROOT, 'untrusted-ref')

    def test_collect_failure_logs_without_user_profiles_or_objects(self):
        for path in ('wasm/ci/run/generation.log', 'wasm/target/runs/link-1/cases/abi/browser.json',
                     'wasm/target/runs/link-1/report.json',
                     'wasm/upstream/run/report.json',
                     'wasm/upstream/run/project/build/wasm/ci/run/generation.log'):
            self.assertTrue(selected(Path(path)), path)
        for path in ('wasm/target/runs/link-1/consumer-chromium/node_modules/private.json',
                     'wasm/target/runs/link-1/cases/abi/profile/Preferences.json',
                     'wasm/target/link/objects/backend/a.json', 'wasm/target/generated/ownership.json',
                     'wasm/upstream/run/project/wasm/package.json',
                     'wasm/upstream/run/project/postgres/src/test/fixtures/data.json'):
            self.assertFalse(selected(Path(path)), path)

    def test_basic_suite_runs_oracle_sql_and_installed_package(self):
        names = [name for name, _ in stages('basic')]
        self.assertLess(names.index('oracle-build'), names.index('oracle-install'))
        self.assertLess(names.index('oracle-install'), names.index('sql'))
        self.assertEqual(names[-1], 'package')

    def test_oracle_is_isolated_and_failed_sql_stops_publication(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root/'buildsys').mkdir()
            (root/'buildsys/wasm_ci.py').write_text('driver')
            native = root/'build/reference'; native.mkdir(parents=True)
            (native/'sentinel').write_text('existing native build')
            config = SimpleNamespace(root=root, build=root/'build', deps=root/'.deps',
                sdk=root/'.deps/sdk', profile_path=root/'profile.json', target='wasm32-emscripten-pthreads',
                mode='debug', jobs=2, node_override='', browser_override='', external_sdk=False)
            commands = []
            def execute(command, *args, **kwargs):
                commands.append(command)
                if 'sql' == Path(args[0]).stem:
                    raise WasmError('injected SQL failure')
                if 'reference-build' in command:
                    assignment = next(value for value in command if value.startswith('REFERENCE_BUILD='))
                    self.assertNotEqual(Path(assignment.split('=',1)[1]), native)
            with patch('wasm_ci.capture', return_value='a'*40), patch('wasm_ci.run', side_effect=execute):
                with self.assertRaisesRegex(WasmError, 'injected SQL failure'):
                    run_suite(config, 'basic', 'emscripten')
            report = json.loads(next((root/'build/wasm/ci').glob('*/report.json')).read_text())
            self.assertEqual(report['status'], 'failed')
            self.assertEqual(report['stages'][-1]['name'], 'sql')
            self.assertFalse(any('wasm-package-check' in command for command in commands))
            self.assertEqual((native/'sentinel').read_text(), 'existing native build')

if __name__ == '__main__': unittest.main()
