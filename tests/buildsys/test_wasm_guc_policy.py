"""Reviewed GUC availability preserves exact ownership checks across revisions."""
from pathlib import Path
import sys
import unittest
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'buildsys'))
from check_guc_policy import policy_owners, validate_alignment, PolicyError

class ConditionalGUCTests(unittest.TestCase):
    def policy(self):
        return {'schema_version':1,'kind':'postgamma.guc-ownership',
                'parameters':{'existing':'session'},
                'conditional_parameters':{'new':{'owner':'session','condition':'Introduced upstream after the pinned revision.'}}}

    def test_only_present_reviewed_gucs_enter_generated_state(self):
        for names in [{'existing'}, {'existing','new'}]:
            owners=policy_owners(self.policy(),names)
            self.assertEqual(set(owners),names)
            validate_alignment(names,owners)
        self.assertEqual(policy_owners(self.policy()), {'existing':'session'})

    def test_unreviewed_and_stale_gucs_still_fail(self):
        for names in [{'existing','unknown'}, {'new'}]:
            with self.assertRaises(PolicyError):
                validate_alignment(names,policy_owners(self.policy(),names))

    def test_overlapping_or_unexplained_conditional_ownership_fails(self):
        for entry in [{'owner':'global','condition':'future'}, {'owner':'session','condition':''}]:
            policy=self.policy();policy['conditional_parameters']['new']=entry
            with self.assertRaises(PolicyError):policy_owners(policy,{'existing'})
        policy=self.policy();policy['conditional_parameters']['existing']={'owner':'role','condition':'duplicate'}
        with self.assertRaises(PolicyError):policy_owners(policy,{'existing'})

if __name__=='__main__':unittest.main()
