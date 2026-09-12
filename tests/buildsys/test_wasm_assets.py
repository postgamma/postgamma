"""Keep release version mapping and core asset identity deterministic."""
from pathlib import Path
from types import SimpleNamespace
import sys
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'buildsys'))
from wasm_assets import npm_version, generate
from wasm_toolchain import digest


class AssetTests(unittest.TestCase):
    def test_canonical_release_versions_map_to_npm_semver(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for source, expected in [('0.1.0a1', '0.1.0-alpha.1'), ('1.2.3b2', '1.2.3-beta.2'),
                                     ('1.2.3rc4', '1.2.3-rc.4'), ('1.2.3', '1.2.3')]:
                (root / 'VERSION').write_text(source + '\n')
                self.assertEqual(npm_version(root), expected)

    def test_identity_changes_with_assets_and_has_no_self_hash_cycle(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary)
            site = output / 'site'
            (site / 'src/loader').mkdir(parents=True)
            (site / 'postgamma.wasm').write_bytes(b'first kernel')
            builder = SimpleNamespace(output=output, root=ROOT, extensions=(),
                config=SimpleNamespace(target='wasm32-emscripten-pthreads', mode='debug'),
                inputs={'postgres_commit': 'pinned'}, report={'bridge': {'sha256': 'bridge', 'flags': []}})
            first = generate(builder)
            self.assertEqual(generate(builder), first)
            self.assertEqual(first['files']['src/loader/identity.js']['sha256'], digest(site / 'src/loader/identity.js'))
            self.assertNotIn('postgamma.assets.json', first['files'])
            (site / 'postgamma.wasm').write_bytes(b'changed kernel')
            self.assertNotEqual(generate(builder)['buildId'], first['buildId'])


if __name__ == '__main__': unittest.main()
