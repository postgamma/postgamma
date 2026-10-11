"""Browser documentation must reject mixed or unsafe runtime packages."""
import hashlib
import io
import json
from pathlib import Path
import sys
import tarfile
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'buildsys'))
import docs_wasm


class BrowserDocumentationTests(unittest.TestCase):
    def package(self, root):
        root.mkdir()
        for name in ('src/index.js', 'src/loader/identity.js', 'worker.js', 'postgamma.wasm', 'LICENSE', 'NOTICE'):
            path = root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text('fixture')
        (root / 'package.json').write_text(json.dumps({'name': '@postgamma/wasm', 'version': '0.1.0-alpha.1'}))
        (root / 'postgamma.assets.json').write_text(json.dumps({
            'version': '0.1.0-alpha.1', 'buildId': 'test-build', 'bridge': {'testing': False},
            'entries': {'worker': 'worker.js', 'wasm': 'postgamma.wasm'},
            'files': {'postgamma.wasm': {'bytes': 7, 'sha256': hashlib.sha256(b'fixture').hexdigest()}},
        }))
        return root

    def test_tampered_package_preserves_previous_staged_runtime(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            package = self.package(root / 'package')
            info = docs_wasm.stage(root / 'build', str(package))
            self.assertTrue(info['available'])
            (package / 'postgamma.wasm').write_text('wrong')
            with self.assertRaisesRegex(ValueError, 'integrity mismatch'):
                docs_wasm.stage(root / 'build', str(package))
            self.assertEqual((root / 'build/docs/browser/assets/wasm/postgamma.wasm').read_text(), 'fixture')

    def test_archive_cannot_escape_the_generated_directory(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for name, link in [('package/../../escape', False), ('package/link', True)]:
                archive = root / 'bad.tgz'
                with tarfile.open(archive, 'w:gz') as tar:
                    member = tarfile.TarInfo(name)
                    if link:
                        member.type = tarfile.SYMTYPE
                        member.linkname = '/tmp'
                        tar.addfile(member)
                    else:
                        member.size = 1
                        tar.addfile(member, io.BytesIO(b'x'))
                with self.assertRaises(ValueError):
                    docs_wasm.stage(root / 'build', str(archive))
            self.assertFalse((root / 'escape').exists())

    def test_testing_bridge_is_not_published_as_a_live_demo(self):
        with tempfile.TemporaryDirectory() as temporary:
            package = self.package(Path(temporary) / 'package')
            path = package / 'postgamma.assets.json'
            manifest = json.loads(path.read_text())
            manifest['bridge']['testing'] = True
            path.write_text(json.dumps(manifest))
            with self.assertRaisesRegex(ValueError, 'production'):
                docs_wasm.verify(package)


if __name__ == '__main__':
    unittest.main()
