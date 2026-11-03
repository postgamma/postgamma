"""Target source adaptation preserves upstream identity and AST byte offsets."""
from pathlib import Path
import sys
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT/'buildsys'))
from wasm_source import adaptations, apply_adaptations, MARKER, RULES, HEADER, GUC_SOURCE, SHMEM_HEADER, FD_SOURCE
from wasm_toolchain import WasmError

class WasmSourceTests(unittest.TestCase):
    def fixture(self, tree):
        for name, (marker, _, _) in RULES.items():
            if name == HEADER: continue
            path = tree/name; path.parent.mkdir(parents=True,exist_ok=True)
            path.write_text(marker + '\n')

    def test_capture_and_generated_sources_receive_identical_adaptation(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            header = '#if defined(__x86_64__)\n/* host */\n' + MARKER + '\nint following;\n'
            trees = [root/name for name in ('upstream','capture','generated')]
            for tree in trees:
                (tree/'src/include').mkdir(parents=True)
                (tree/'src/include/c.h').write_text(header)
                self.fixture(tree)
            changes = adaptations(trees[0])
            for tree in trees[1:]: apply_adaptations(tree, changes)
            for name in RULES:
                self.assertEqual((trees[1]/name).read_bytes(), (trees[2]/name).read_bytes())
            self.assertEqual((trees[0]/'src/include/c.h').read_text(), header)
            self.assertIn('#elif defined(__wasm__)', (trees[1]/'src/include/c.h').read_text())
            with self.assertRaisesRegex(WasmError, 'input changed'):
                apply_adaptations(trees[1], changes)
            self.assertEqual(adaptations(trees[1]), [])

    def test_older_sources_and_unreviewed_architecture_shapes(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary); (root/'src/include').mkdir(parents=True)
            self.fixture(root)
            header = root/'src/include/c.h'
            header.write_text('/* No architecture normalization in this revision. */\n')
            self.assertEqual({c['path'] for c in adaptations(root)}, {GUC_SOURCE, SHMEM_HEADER, FD_SOURCE})
            header.write_text('#error "cannot identify target architecture"\n')
            with self.assertRaisesRegex(WasmError, 'unreviewed'):
                adaptations(root)

    def test_installer_retains_an_exact_guard_check_after_target_adaptation(self):
        from wasm_generate import project_adapter
        from wasm_source import SHMEM_REPLACEMENT
        from wasm_toolchain import read_json
        adapter = read_json(ROOT/'manifests/postgresql/adapter.json')
        original = next(row for row in adapter['generated_support']['edits'] if row['id']=='shmem.target-default')
        self.assertEqual(project_adapter(adapter, []), adapter)
        projected = project_adapter(adapter, [{'path': SHMEM_HEADER}])
        guard = next(row for row in projected['generated_support']['edits'] if row['id']=='shmem.target-default')
        self.assertEqual(guard, dict(original, match=SHMEM_REPLACEMENT.splitlines()[0]))
        self.assertNotEqual(original['match'], guard['match'])
        original['expected_matches'] = 0
        with self.assertRaisesRegex(WasmError, 'unreviewed'):
            project_adapter(adapter, [{'path': SHMEM_HEADER}])

    def test_all_presync_fallbacks_are_disabled_only_on_emscripten(self):
        import os
        import shlex
        import subprocess
        from wasm_source import FLUSH_REPLACEMENT
        # Preprocess the actual source adaptation with every flush provider
        # advertised. In particular, MS_ASYNC must not restore the presync pass
        # after configure disables POSIX_FADVISE.
        compiler = shlex.split(os.environ.get('CC', 'cc'))
        providers = '#define HAVE_SYNC_FILE_RANGE 1\n#define MS_ASYNC 1\n#define USE_POSIX_FADVISE 1\n#define POSIX_FADV_DONTNEED 1\n'
        for target, expected in [('', True), ('#define __EMSCRIPTEN__ 1\n', False)]:
            command = [*compiler, '-E', '-dM', '-x', 'c', '-']
            result = subprocess.run(command, input=providers + target + FLUSH_REPLACEMENT,
                                    text=True, capture_output=True)
            self.assertEqual(result.returncode, 0, (command, result.stdout, result.stderr))
            self.assertEqual('#define PG_FLUSH_DATA_WORKS 1' in result.stdout, expected)

if __name__ == '__main__': unittest.main()
