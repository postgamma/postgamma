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
from plan_wasm_ci import plan, shared_input, platform_input
from collect_wasm_evidence import selected
from wasm_ci import run_suite, stages
from wasm_toolchain import WasmError

class WasmCITests(unittest.TestCase):
    def test_shared_changes_require_native_regression(self):
        for path in ('runtime/platform/emscripten/storage.cpp', 'runtime/src/session.c',
                     'compiler/src/transform.cpp', 'embedded-c/src/api.c',
                     'manifests/thread-state.json', 'buildsys/apply_replacements.py', 'postgres',
                     '.github/workflows/wasm.yml'):
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
                     'wasm/lifecycle/run/report.json',
                     'wasm/lifecycle/run/firefox-fatal-opfs-abort/browser.json',
                     'wasm/lifecycle/run/webkit-fatal-opfs-trap/browser.log',
                     'wasm/upstream/run/report.json',
                     'wasm/upstream/run/project/build/wasm/ci/run/generation.log'):
            self.assertTrue(selected(Path(path)), path)
        for path in ('wasm/target/runs/link-1/consumer-chromium/node_modules/private.json',
                     'wasm/lifecycle/run/profile/Preferences.json',
                     'wasm/target/runs/link-1/cases/abi/profile/Preferences.json',
                     'wasm/target/link/objects/backend/a.json', 'wasm/target/generated/ownership.json',
                     'wasm/upstream/run/project/wasm/package.json',
                     'wasm/upstream/run/project/postgres/src/test/fixtures/data.json'):
            self.assertFalse(selected(Path(path)), path)

    def test_basic_suite_runs_oracle_sql_and_installed_package(self):
        names = [name for name, _ in stages('basic')]
        self.assertLess(names.index('generation'), names.index('runtime'))
        self.assertLess(names.index('runtime'), names.index('sql'))
        self.assertLess(names.index('oracle-build'), names.index('oracle-install'))
        self.assertLess(names.index('oracle-install'), names.index('sql'))
        self.assertEqual(names[-1], 'package')

    def test_release_and_weekly_require_lifecycle_before_package(self):
        names = [name for name, _ in stages('weekly')]
        self.assertLess(names.index('storage'), names.index('lifecycle'))
        self.assertLess(names.index('lifecycle'), names.index('package'))

    def test_platform_changes_select_probes_and_full_suite_always_runs_them(self):
        for path in ('runtime/platform/emscripten/io.c', 'wasm/bridge/sql.c',
                     'tests/runtime/test_path_runtime.c', 'tests/wasm/probes/recovery.c',
                     'manifests/profiles/wasm-emscripten.json', 'wasm/package-lock.json',
                     '.github/actions/wasm-setup/action.yml'):
            self.assertTrue(platform_input(path), path)
        for path in ('wasm/src/api/database.ts', 'docs/getting-started/browser.md',
                     'tests/wasm/sql/concurrency.ts'):
            self.assertFalse(platform_input(path), path)
        self.assertTrue(plan(ROOT, '')['platform'])
        with patch('plan_wasm_ci.subprocess.check_output', return_value=b'wasm/package-lock.json\0'):
            result = plan(ROOT, 'a'*40)
            self.assertTrue(result['platform'])
            self.assertFalse(result['native'])
        for profile, explicit in [('weekly', False), ('weekly', True), ('basic', True)]:
            names = [name for name, _ in stages(profile, explicit)]
            for gate in ('runtime', 'platform', 'wait', 'recovery'):
                self.assertLess(names.index(gate), names.index('sql'))
                self.assertEqual(names.count(gate), 1)
        self.assertNotIn('platform', dict(stages('basic')))

    def test_failed_platform_probe_stops_sql_and_package(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root/'buildsys').mkdir()
            (root/'buildsys/wasm_ci.py').write_text('driver')
            config = SimpleNamespace(root=root, build=root/'build', deps=root/'.deps',
                profile_path=root/'profile.json', target='wasm32-emscripten-pthreads',
                mode='release', jobs=2, node_override='', browser_override='', external_sdk=False)
            commands = []
            def execute(command, log, *args, **kwargs):
                commands.append(command)
                if Path(log).stem == 'wait':
                    raise WasmError('injected lost wakeup')
            with patch('wasm_ci.capture', return_value='a'*40), patch('wasm_ci.run', side_effect=execute):
                with self.assertRaisesRegex(WasmError, 'injected lost wakeup'):
                    run_suite(config, 'weekly', 'emscripten')
            report = json.loads(next((root/'build/wasm/ci').glob('*/report.json')).read_text())
            self.assertTrue(report['platform_checks'])
            self.assertEqual(report['stages'][-1]['name'], 'wait')
            self.assertEqual(report['stages'][-1]['status'], 'failed')
            self.assertFalse(any('wasm-sql-check' in c or 'wasm-package-check' in c for c in commands))

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
