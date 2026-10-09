"""Native memory gates require an unchanged Mozilla build without Juggler."""
import hashlib
import io
import json
from pathlib import Path
import sys
import struct
import tarfile
import tempfile
import unittest
from unittest import mock
import zipfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'buildsys'))
import wasm_firefox as firefox
from wasm_toolchain import WasmError


class NativeFirefoxTests(unittest.TestCase):
    def bundle(self, version='155.0', juggler=False, optimized=False):
        omni = io.BytesIO()
        with zipfile.ZipFile(omni, 'w') as archive:
            archive.writestr('chrome/juggler/content/Runtime.js' if juggler else 'chrome/toolkit/content/hello.js', 'source')
        omni_data = omni.getvalue()
        if optimized:
            end = bytearray(omni_data[-22:])
            directory_offset = struct.unpack_from('<I', end, 16)[0]
            directory = bytearray(omni_data[directory_offset:-22])
            prefix_size = 4 + len(directory) + len(end)
            struct.pack_into('<I', directory, 42, prefix_size)
            struct.pack_into('<I', end, 16, 4)
            omni_data = struct.pack('<I', prefix_size) + directory + end + omni_data[:directory_offset] + end
        files = {'firefox/firefox': b'#!/bin/sh\nexit 0\n', 'firefox/libxul.so': b'engine',
                 'firefox/omni.ja': omni_data, 'firefox/application.ini':
                 ('[App]\nVersion=' + version + '\nSourceStamp=' + firefox.PIN['sourceStamp'] +
                  '\nBuildID=' + firefox.PIN['buildId'] + '\n').encode()}
        output = io.BytesIO()
        with tarfile.open(fileobj=output, mode='w:xz') as archive:
            for name, data in files.items():
                member = tarfile.TarInfo(name); member.size = len(data); member.mode = 0o755
                archive.addfile(member, io.BytesIO(data))
        return output.getvalue()

    def install(self, deps, data, checksum=None):
        pin = dict(firefox.PIN, bytes=len(data), sha512=checksum or hashlib.sha512(data).hexdigest())
        with mock.patch.object(firefox, 'PIN', pin), \
                mock.patch.object(firefox.platform, 'system', return_value='Linux'), \
                mock.patch.object(firefox.platform, 'machine', return_value='x86_64'), \
                mock.patch.object(firefox.urllib.request, 'urlopen', return_value=io.BytesIO(data)) as fetch:
            result = firefox.install(deps)
            self.assertEqual(firefox.install(deps), result)
            fetch.assert_called_once()
            return result, pin

    def test_archive_identity_and_installed_engine_are_checked(self):
        with tempfile.TemporaryDirectory() as temporary:
            deps = Path(temporary)
            (executable, receipt), pin = self.install(deps, self.bundle())
            self.assertTrue(receipt.is_file())
            (executable.parent / 'libxul.so').write_bytes(b'changed engine')
            with mock.patch.object(firefox, 'PIN', pin), \
                    mock.patch.object(firefox.platform, 'system', return_value='Linux'), \
                    mock.patch.object(firefox.platform, 'machine', return_value='x86_64'):
                with self.assertRaisesRegex(WasmError, 'cache changed'):
                    firefox.install(deps)

    def test_update_policy_is_separate_from_original_engine_files(self):
        with tempfile.TemporaryDirectory() as temporary:
            (executable, receipt), _ = self.install(Path(temporary), self.bundle())
            state = json.loads(receipt.read_text())
            self.assertNotIn(firefox.POLICY_NAME, state['files'])
            self.assertEqual(state['configuration']['value'], {'policies': {'DisableAppUpdate': True}})
            policy = executable.parent / 'distribution/policies.json'
            self.assertEqual(json.loads(policy.read_text()), state['configuration']['value'])

    def test_changed_policy_and_pending_updates_fail_closed(self):
        for changed in ('policy', 'pending-update'):
            with self.subTest(changed=changed), tempfile.TemporaryDirectory() as temporary:
                deps = Path(temporary)
                (executable, _), pin = self.install(deps, self.bundle())
                if changed == 'policy':
                    (executable.parent / 'distribution/policies.json').write_text('{}')
                else:
                    (executable.parent / 'updated').mkdir()
                with mock.patch.object(firefox, 'PIN', pin), \
                        mock.patch.object(firefox.platform, 'system', return_value='Linux'), \
                        mock.patch.object(firefox.platform, 'machine', return_value='x86_64'):
                    with self.assertRaisesRegex(WasmError, 'policy changed|staged automatic updates'):
                        firefox.install(deps)

    def test_old_receipt_can_record_policy_without_changing_engine_inventory(self):
        with tempfile.TemporaryDirectory() as temporary:
            deps = Path(temporary)
            (executable, receipt), pin = self.install(deps, self.bundle())
            state = json.loads(receipt.read_text())
            del state['configuration']
            receipt.write_text(json.dumps(state))
            with mock.patch.object(firefox, 'PIN', pin), \
                    mock.patch.object(firefox.platform, 'system', return_value='Linux'), \
                    mock.patch.object(firefox.platform, 'machine', return_value='x86_64'):
                self.assertEqual(firefox.install(deps), (executable, receipt))
            after = json.loads(receipt.read_text())
            self.assertEqual(after['files'], state['files'])
            self.assertEqual(after['configuration']['value'], firefox.POLICY)

    def test_bad_download_is_rejected_before_extraction(self):
        with tempfile.TemporaryDirectory() as temporary:
            deps = Path(temporary)
            with self.assertRaisesRegex(WasmError, 'SHA512 mismatch'):
                self.install(deps, self.bundle(), checksum='0' * 128)
            self.assertFalse(list(deps.rglob('receipt.json')))
            self.assertFalse(list(deps.rglob('*.partial')))

    def test_optimized_omni_layout_does_not_require_python_zip_compatibility(self):
        for juggler in (False, True):
            with self.subTest(juggler=juggler), tempfile.TemporaryDirectory() as temporary:
                data = self.bundle(optimized=True, juggler=juggler)
                if juggler:
                    with self.assertRaisesRegex(WasmError, 'must not contain Playwright Juggler'):
                        self.install(Path(temporary), data)
                else:
                    self.install(Path(temporary), data)

    def test_wrong_version_and_juggler_archive_cannot_qualify(self):
        for data, message in ((self.bundle(version='154.0'), 'source identity mismatch'),
                              (self.bundle(juggler=True), 'must not contain Playwright Juggler')):
            with self.subTest(message=message), tempfile.TemporaryDirectory() as temporary:
                deps = Path(temporary)
                with self.assertRaisesRegex(WasmError, message):
                    self.install(deps, data)
                self.assertFalse(list(deps.rglob('receipt.json')))


if __name__ == '__main__': unittest.main()
