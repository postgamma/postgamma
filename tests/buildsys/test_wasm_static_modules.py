"""Candidate extensions retain registration checks against their compiled major."""
from pathlib import Path
import copy
import sys
import unittest
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'buildsys'))
from wasm_link import target_static_modules
from wasm_toolchain import WasmError, read_json
from bundle_embedded_static_modules import StaticModuleBundleError

class StaticModuleTargetTests(unittest.TestCase):
    def test_candidate_uses_header_major_without_mutating_the_product_contract(self):
        document=read_json(ROOT/'manifests/extensions/embedded-static.json')
        original=copy.deepcopy(document)
        for major,candidate in ((19,False),(20,True)):
            target=target_static_modules(document,('dict_snowball','plpgsql','pgvector'),major,candidate)
            self.assertEqual(target['postgresql_major'],major)
            self.assertEqual({m['postgresql_major'] for m in target['modules']},{major})
            self.assertEqual({m['id'] for m in target['modules']},{'dict_snowball','plpgsql','pgvector'})
        self.assertEqual(document,original)
        with self.assertRaisesRegex(WasmError,'pinned contract'):
            target_static_modules(document,('pgvector',),20,False)

    def test_candidate_does_not_accept_an_inconsistent_source_manifest(self):
        document=read_json(ROOT/'manifests/extensions/embedded-static.json')
        next(m for m in document['modules'] if m['id']=='pgvector')['postgresql_major']=20
        with self.assertRaisesRegex(StaticModuleBundleError,'incompatible'):
            target_static_modules(document,('pgvector',),20,True)

if __name__=='__main__':unittest.main()
