"""Contrib SQL/resources must match the reviewed Wasm state and symbol closure."""
import json
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'buildsys'))
from wasm_contrib import CONTRIB, STANDARD, selection, sql_symbols
from wasm_toolchain import WasmError
from bundle_embedded_static_modules import validate_manifest


class ContribContractTests(unittest.TestCase):
    def test_selection_is_explicit_and_rejects_unknown_modules(self):
        self.assertEqual(selection('standard'), STANDARD)
        self.assertEqual(selection('hstore,pg_trgm'), CONTRIB)
        self.assertEqual(selection('none'), ())
        for value in ('pg_trgm,pg_trgm', 'arbitrary', 'pgvector,'):
            with self.assertRaises(WasmError): selection(value)

    def test_all_sql_entries_and_resources_are_registered(self):
        manifest = validate_manifest(json.loads((ROOT / 'manifests/extensions/wasm-contrib.json').read_text()))
        self.assertEqual({m['id'] for m in manifest['modules']}, set(CONTRIB))
        for module in manifest['modules']:
            source = ROOT / 'postgres/contrib' / module['id']
            self.assertEqual({entry['logical_name'] for entry in module['symbols']}, sql_symbols(source))
            expected = {str(path.relative_to(ROOT)) for path in source.iterdir() if path.suffix in ('.sql', '.control')}
            self.assertEqual({row['source'] for row in module['resources']}, expected)
            self.assertNotIn('process-global-state', module['capabilities'])
        native = json.loads((ROOT / 'manifests/extensions/embedded-static.json').read_text())
        self.assertFalse(set(CONTRIB) & {m['id'] for m in native['modules']})

    def test_trigram_mutable_globals_belong_to_logical_roles(self):
        policy = json.loads((ROOT / 'manifests/ownership/extensions/pg_trgm.json').read_text())
        self.assertEqual({row['id'] for row in policy['decisions']}, {
            'external:similarity_threshold', 'external:word_similarity_threshold',
            'external:strict_word_similarity_threshold', 'external:CMPTRGM'})
        self.assertEqual({row['owner'] for row in policy['decisions']}, {'role'})


if __name__ == '__main__': unittest.main()
