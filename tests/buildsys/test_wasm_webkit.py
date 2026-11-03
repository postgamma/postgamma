"""A browser repair must use the pinned archive and unchanged installed files."""
import hashlib
import io
from pathlib import Path
import sys
import tarfile
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'buildsys'))
import wasm_webkit as webkit
from wasm_toolchain import WasmError


class NativeWebKitTests(unittest.TestCase):
    def bundle(self, revision='322662@main'):
        buffer = io.BytesIO()
        with tarfile.open(fileobj=buffer, mode='w:xz') as archive:
            for name, data in {'README.txt': 'WebKit Revision: ' + revision,
                               'MiniBrowser': '#!/bin/sh\nexit 0\n'}.items():
                value = data.encode()
                member = tarfile.TarInfo(name)
                member.size = len(value)
                member.mode = 0o755 if name == 'MiniBrowser' else 0o644
                archive.addfile(member, io.BytesIO(value))
        return buffer.getvalue()

    def test_install_verifies_archive_and_reuse_rejects_changed_executable(self):
        data = self.bundle()
        pin = dict(webkit.PIN, bytes=len(data), sha256=hashlib.sha256(data).hexdigest())
        with tempfile.TemporaryDirectory() as temporary, mock.patch.object(webkit, 'PIN', pin), \
                mock.patch.object(webkit.platform, 'system', return_value='Linux'), \
                mock.patch.object(webkit.platform, 'machine', return_value='x86_64'), \
                mock.patch.object(webkit.urllib.request, 'urlopen', return_value=io.BytesIO(data)) as fetch:
            executable, receipt = webkit.install(Path(temporary))
            self.assertTrue(receipt.is_file())
            self.assertEqual(webkit.install(Path(temporary)), (executable, receipt))
            fetch.assert_called_once()
            executable.write_text('changed browser')
            with self.assertRaisesRegex(WasmError, 'cache changed'):
                webkit.install(Path(temporary))

    def test_bad_checksum_is_rejected_before_extraction(self):
        data = self.bundle()
        pin = dict(webkit.PIN, bytes=len(data), sha256='0' * 64)
        with tempfile.TemporaryDirectory() as temporary, mock.patch.object(webkit, 'PIN', pin), \
                mock.patch.object(webkit.platform, 'system', return_value='Linux'), \
                mock.patch.object(webkit.platform, 'machine', return_value='x86_64'), \
                mock.patch.object(webkit.urllib.request, 'urlopen', return_value=io.BytesIO(data)):
            with self.assertRaisesRegex(WasmError, 'SHA256 mismatch'):
                webkit.install(Path(temporary))
            self.assertFalse(list(Path(temporary).rglob('MiniBrowser')))
            self.assertFalse(list(Path(temporary).rglob('*.partial')))

    def test_revision_mismatch_is_not_recorded_as_installed(self):
        data = self.bundle('old-build')
        pin = dict(webkit.PIN, bytes=len(data), sha256=hashlib.sha256(data).hexdigest())
        with tempfile.TemporaryDirectory() as temporary, mock.patch.object(webkit, 'PIN', pin), \
                mock.patch.object(webkit.platform, 'system', return_value='Linux'), \
                mock.patch.object(webkit.platform, 'machine', return_value='x86_64'), \
                mock.patch.object(webkit.urllib.request, 'urlopen', return_value=io.BytesIO(data)):
            with self.assertRaisesRegex(WasmError, 'revision mismatch'):
                webkit.install(Path(temporary))
            self.assertFalse(list(Path(temporary).rglob('receipt.json')))


if __name__ == '__main__': unittest.main()
