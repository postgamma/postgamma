"""Committed contrib subtrees retain the same provenance and dirty-tree guard."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class MaterializeTests(unittest.TestCase):
    def test_pinned_subtree_and_fail_closed_inputs(self):
        with tempfile.TemporaryDirectory() as temporary:
            base = Path(temporary)
            repo = base / 'repo'; repo.mkdir()
            def git(*args):
                return subprocess.check_output(['git', '-C', str(repo), *args], text=True).strip()
            git('init', '-q')
            source = repo / 'contrib/example'; source.mkdir(parents=True)
            (source / 'entry.c').write_text('int entry;\n')
            (repo / 'unrelated').write_text('excluded\n')
            git('add', '.')
            git('-c', 'user.name=Test', '-c', 'user.email=test@example.invalid', 'commit', '-qm', 'fixture')
            commit = git('rev-parse', 'HEAD')
            manifest = base / 'upstream.json'
            manifest.write_text(json.dumps({'commit': commit, 'repository': str(repo)}))
            build = base / 'build'; destination = build / 'generated'
            command = [sys.executable, str(ROOT / 'buildsys/materialize.py'), '--repository', str(repo),
                       '--manifest', str(manifest), '--build-root', str(build), '--destination', str(destination)]
            def run(subdirectory):
                return subprocess.run(command + ['--subdirectory', subdirectory], text=True, capture_output=True)
            result = run('contrib/example')
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual((destination / 'entry.c').read_text(), 'int entry;\n')
            self.assertFalse((destination / 'unrelated').exists())
            metadata = json.loads((destination / '.postgamma-source.json').read_text())
            self.assertEqual(metadata['source_commit'], commit)
            self.assertEqual(metadata['source_subdirectory'], 'contrib/example')
            for bad in ('../repo', '/contrib', 'contrib/../contrib', 'contrib//example', 'unrelated'):
                result = run(bad)
                self.assertNotEqual(result.returncode, 0, bad)
                self.assertTrue((destination / 'entry.c').exists(), 'Validation precedes replacement')
            (source / 'entry.c').write_text('uncommitted\n')
            result = run('contrib/example')
            self.assertIn('dirty source tree', result.stderr)
            self.assertEqual((destination / 'entry.c').read_text(), 'int entry;\n')


if __name__ == '__main__': unittest.main()
