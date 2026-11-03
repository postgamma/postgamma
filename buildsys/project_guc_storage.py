"""Apply reviewed storage transitions only when target AST GUC bindings prove them."""
from __future__ import annotations
import copy
from wasm_toolchain import WasmError


def project(policy: dict, catalog: dict, inventory: dict, manifest: dict) -> dict:
    if manifest.get('schema_version') != 1 or manifest.get('kind') != 'postgamma.backend-guc-transitions':
        raise WasmError('invalid GUC storage transition manifest')
    result = copy.deepcopy(policy)
    decisions = {row['id']: row for row in result['decisions']}
    candidates = {row['id']: row for row in catalog['candidates']}
    bindings = {}
    for row in inventory['declarations']:
        if row.get('definition') is True:
            bindings.setdefault(row['usr'], set()).add(row['symbol_id'])
    seen = set()
    for transition in manifest['transitions']:
        identifier = transition['id']
        if identifier in seen or identifier not in decisions or not transition.get('rationale'):
            raise WasmError('invalid or duplicate GUC storage transition: ' + identifier)
        seen.add(identifier)
        decision = decisions[identifier]
        if decision['owner'] != transition['previous_owner']:
            raise WasmError('GUC storage transition baseline owner changed: ' + identifier)
        candidate = candidates.get(identifier)
        binding = bindings.get(candidate['usr']) if candidate else None
        if binding:
            if binding != {'pg.guc.' + transition['parameter']}:
                raise WasmError('unexpected GUC binding for storage transition: ' + identifier)
            decision.update(owner='managed_guc', rationale=transition['rationale'])
    return result
