"""Version-specific GUC storage transitions require exact AST binding evidence."""
from pathlib import Path
import sys
import unittest
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'buildsys'))
from project_guc_storage import project
from wasm_toolchain import WasmError

class GUCStorageTests(unittest.TestCase):
    def inputs(self):
        return ({'decisions':[{'id':'external:alias','owner':'role','rationale':'Old hook alias'}]},
                {'candidates':[{'id':'external:alias','usr':'storage'}]},
                {'declarations':[]},
                {'schema_version':1,'kind':'postgamma.backend-guc-transitions','transitions':[
                    {'id':'external:alias','parameter':'parameter','previous_owner':'role','rationale':'Direct GUC storage'}]})

    def test_old_alias_stays_owned_and_new_direct_binding_is_managed(self):
        policy,catalog,inventory,manifest=self.inputs()
        self.assertEqual(project(policy,catalog,inventory,manifest),policy)
        inventory['declarations']=[{'definition':True,'usr':'storage','symbol_id':'pg.guc.parameter'}]
        self.assertEqual(project(policy,catalog,inventory,manifest)['decisions'][0]['owner'],'managed_guc')
        self.assertEqual(policy['decisions'][0]['owner'],'role')

    def test_declaration_only_or_other_storage_does_not_change_owner(self):
        policy,catalog,inventory,manifest=self.inputs()
        for declaration in [{'definition':False,'usr':'storage','symbol_id':'pg.guc.parameter'},
                            {'definition':True,'usr':'other','symbol_id':'pg.guc.parameter'}]:
            inventory['declarations']=[declaration]
            self.assertEqual(project(policy,catalog,inventory,manifest),policy)

    def test_conflicting_bindings_are_rejected(self):
        policy,catalog,inventory,manifest=self.inputs()
        inventory['declarations']=[{'definition':True,'usr':'storage','symbol_id':'pg.guc.other'},
                                   {'definition':True,'usr':'storage','symbol_id':'pg.guc.parameter'}]
        with self.assertRaisesRegex(WasmError,'unexpected GUC binding'):project(policy,catalog,inventory,manifest)

    def test_changed_parameter_or_baseline_owner_is_rejected(self):
        policy,catalog,inventory,manifest=self.inputs()
        inventory['declarations']=[{'definition':True,'usr':'storage','symbol_id':'pg.guc.other'}]
        with self.assertRaisesRegex(WasmError,'unexpected GUC binding'):project(policy,catalog,inventory,manifest)
        inventory['declarations']=[];policy['decisions'][0]['owner']='instance'
        with self.assertRaisesRegex(WasmError,'baseline owner changed'):project(policy,catalog,inventory,manifest)

if __name__=='__main__':unittest.main()
