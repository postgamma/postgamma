"""Alpha evidence must describe the exact production archive and full duration."""
import copy
import json
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'buildsys'))
from wasm_alpha import qualify, validate_endurance
from wasm_contrib import STANDARD
from wasm_package import verify_tree
from wasm_soak import ENGINES, production_package
from wasm_toolchain import WasmError, digest


class AlphaReceiptTests(unittest.TestCase):
    def test_partial_different_or_instrumented_endurance_cannot_qualify(self):
        archive = dict(path='postgamma.tgz', sha256='a' * 64)
        report = dict(status='passed', mode='release', growthMiB=128,
                      productionPackage=dict(archive=archive, bridgeTesting=False),
                      parameters=dict(seconds=7200, rounds=0, faultSource='control-worker'),
                      engines={name: dict(status='passed', complete=dict(elapsedMs=7200000),
                                          resourceGate=dict(status='passed')) for name in ENGINES})
        validate_endurance(report, archive, 7200)
        changes = [('status', 'failed'), ('mode', 'debug'), ('growthMiB', 129)]
        for field, value in changes:
            altered = copy.deepcopy(report); altered[field] = value
            with self.subTest(field=field), self.assertRaises(WasmError):
                validate_endurance(altered, archive, 7200)
        altered = copy.deepcopy(report); altered['engines'].pop('firefox')
        with self.assertRaises(WasmError): validate_endurance(altered, archive, 7200)
        for field, value in [('seconds', 7199), ('rounds', 150), ('faultSource', 'native-pthread')]:
            altered = copy.deepcopy(report); altered['parameters'][field] = value
            with self.subTest(parameter=field), self.assertRaises(WasmError):
                validate_endurance(altered, archive, 7200)
        for name in ENGINES:
            for field, value in [('status', 'not-run'), ('elapsedMs', 7199999), ('resource', 'not-run')]:
                altered = copy.deepcopy(report); row = altered['engines'][name]
                if field == 'elapsedMs': row['complete'][field] = value
                elif field == 'resource': row['resourceGate']['status'] = value
                else: row[field] = value
                with self.subTest(engine=name, field=field), self.assertRaises(WasmError):
                    validate_endurance(altered, archive, 7200)
        for package in (dict(archive=archive, bridgeTesting=True),
                        dict(archive=dict(archive, sha256='b' * 64), bridgeTesting=False)):
            altered = copy.deepcopy(report); altered['productionPackage'] = package
            with self.assertRaises(WasmError): validate_endurance(altered, archive, 7200)

    def test_short_or_debug_requests_are_rejected_before_building(self):
        for mode, seconds in [('debug', 7200), ('release', 7199), ('release', 28801)]:
            with self.subTest(mode=mode, seconds=seconds), self.assertRaises(WasmError):
                qualify(SimpleNamespace(mode=mode), 'emscripten', seconds)

    def test_production_archive_and_sources_are_reverified(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary); package = root / 'package'; package.mkdir()
            required = ('src/index.js', 'src/index.d.ts', 'LICENSE', 'NOTICE',
                        'licenses/LICENSE.postgresql', 'licenses/LICENSE.emscripten', 'licenses/LICENSE.musl')
            for name in required:
                path = package / name; path.parent.mkdir(parents=True, exist_ok=True); path.write_text(name)
            (package / 'package.json').write_text(json.dumps(dict(version='0.1.0-alpha.1')))
            manifest = dict(version='0.1.0-alpha.1', buildId='candidate', mode='release',
                            bridge=dict(testing=False), extensions=list(STANDARD), files={})
            manifest_path = package / 'postgamma.assets.json'; manifest_path.write_text(json.dumps(manifest))
            archive = root / 'candidate.tgz'; archive.write_bytes(b'frozen archive')
            source = root / 'source.ts'; source.write_text('candidate source')
            receipt = dict(status='passed', consumer_validated=True, package=str(package),
                           staged=verify_tree(package, str(root)), sources={'source.ts': digest(source)},
                           archive=dict(path=str(archive), sha256=digest(archive)))
            receipt_path = root / 'receipt.json'; receipt_path.write_text(json.dumps(receipt))
            config = SimpleNamespace(root=root, mode='release')
            self.assertEqual(production_package(config, receipt_path), receipt)
            for path in (archive, source, package / 'src/index.js'):
                original = path.read_bytes(); path.write_bytes(original + b'changed')
                with self.subTest(path=path.name), self.assertRaises(WasmError):
                    production_package(config, receipt_path)
                path.write_bytes(original)
            receipt['consumer_validated'] = False; receipt_path.write_text(json.dumps(receipt))
            with self.assertRaises(WasmError): production_package(config, receipt_path)


if __name__ == '__main__': unittest.main()
