"""Cross-language drift and unreviewed SDK internals must stop a build."""
import copy
import json
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'buildsys'))
from check_wasm_sdk_contract import check as check_sdk
from wasm_storage_protocol import check, render, MANIFEST, HEADER, MODULE
from wasm_toolchain import WasmError, digest


class WasmContractTests(unittest.TestCase):
    def test_checked_in_interfaces_match_and_cannot_drift_independently(self):
        check(ROOT)
        contract = json.loads((ROOT / MANIFEST).read_text())
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / MANIFEST).parent.mkdir(parents=True)
            (root / MANIFEST).write_text(json.dumps(contract))
            check(root, write=True)
            for name in (HEADER, MODULE):
                with self.subTest(name=name):
                    original = (root / name).read_text()
                    (root / name).write_text(original.replace(' = 1,', ' = 99,').replace('"lookup": 1', '"lookup": 99'))
                    with self.assertRaisesRegex(WasmError, 'stale storage contract'):
                        check(root)
                    (root / name).write_text(original)
            contract['operations']['read'] = contract['operations']['write']
            with self.assertRaisesRegex(WasmError, 'numbering'):
                render(contract)

    def test_sdk_source_change_missing_file_and_new_version_are_rejected(self):
        contract = json.loads((ROOT / 'manifests/profiles/wasm-sdk-contract.json').read_text())
        profile = json.loads((ROOT / 'manifests/profiles/wasm-emscripten.json').read_text())
        self.assertEqual(contract['emscripten_version'], profile['emsdk']['version'])
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            sdk = root / 'sdk'
            fixture = copy.deepcopy(contract)
            for name in fixture['files']:
                source = sdk / 'upstream/emscripten' / name
                source.parent.mkdir(parents=True, exist_ok=True)
                source.write_text('reviewed SDK behavior')
                fixture['files'][name] = digest(source)
            path = root / 'manifests/profiles/wasm-sdk-contract.json'
            path.parent.mkdir(parents=True)
            path.write_text(json.dumps(fixture))
            check_sdk(root, sdk, contract['emscripten_version'])
            with self.assertRaisesRegex(WasmError, 'requires reviewed'):
                check_sdk(root, sdk, 'next-version')
            for name in fixture['files']:
                source = sdk / 'upstream/emscripten' / name
                source.write_text('changed mailbox or queue behavior')
                with self.assertRaisesRegex(WasmError, 'SDK adapter source changed'):
                    check_sdk(root, sdk, contract['emscripten_version'])
                source.unlink()
                with self.assertRaisesRegex(WasmError, 'SDK adapter source changed'):
                    check_sdk(root, sdk, contract['emscripten_version'])
                source.write_text('reviewed SDK behavior')
