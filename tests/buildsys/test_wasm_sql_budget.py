"""Keep SQL limits shared by the bridge and JavaScript, and require full evidence."""
import copy
import json
from pathlib import Path
import sys
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'buildsys'))
from wasm_sql_budget import generate_policy, summarize
from wasm_toolchain import WasmError

class SqlBudgetTests(unittest.TestCase):
    def policy(self):
        return json.loads((ROOT / 'manifests/profiles/wasm-resource-budget.json').read_text())

    def test_one_policy_generates_both_byte_and_connection_limits(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory)
            policy = self.policy()
            generate_policy(ROOT, output, policy)
            self.assertIn('#define PGW_MAX_CONNECTIONS 4U', (output / 'wasm-sql-policy.h').read_text())
            module = (output / 'site/src/protocol/limits.js').read_text()
            limits = json.loads(module.split('Object.freeze(', 1)[1].split(');', 1)[0])
            self.assertEqual(limits, policy['sql_limits'])
            policy['sql_limits']['maxConnections'] += 1
            with self.assertRaisesRegex(WasmError, 'session budget'):
                generate_policy(ROOT, output, policy)

    def test_partial_or_failed_scenarios_cannot_freeze_the_profile(self):
        from wasm_sql_budget import SCENARIOS
        with self.assertRaisesRegex(WasmError, 'every SQL scenario'):
            summarize(self.policy(), {}, {})
        reports = {name: {'status': 'passed', 'probe': {'scenario': name, 'checks': ['ok'], 'metrics': []}}
                   for name in SCENARIOS}
        reports['results']['status'] = 'failed'
        with self.assertRaisesRegex(WasmError, 'results'):
            summarize(self.policy(), {}, reports)

if __name__ == '__main__':
    unittest.main()
