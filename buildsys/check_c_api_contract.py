#!/usr/bin/env python3
"""Validate the public API contract against compiler-derived facts."""

from __future__ import annotations

import argparse
import json
import re
from collections import deque
from pathlib import Path
from typing import Any


POLICY_KIND = "postgamma.c-public-api-contract"
CATALOG_KIND = "postgamma.public-api-ast-catalog"
PROTOCOL_KIND = "postgamma.postgresql-protocol-contract"
BASELINE_KIND = "postgamma.core-abi-baseline"
REQUIRED_CONTRACT_FIELDS = frozenset(
    {
        "ownership",
        "blocking",
        "thread_safety",
        "callback_context",
        "errors",
        "allowed_states",
    }
)
PHASE_RANK = {
    "cluster-lifecycle": 1,
    "session-execution": 2,
    "c-api-foundation": 3,
    "ordered-results": 4,
    "chunked-results": 5,
    "copy-streaming": 6,
    "runtime-events": 7,
    "arrow-management": 8,
    "logical-management": 9,
    "embedded-release": 10,
    "post-v1": 11,
}
IDENTIFIER = re.compile(r"\b[A-Za-z_][A-Za-z0-9_]*\b")
PUBLIC_RECORD = re.compile(r"\bstruct (pgm_[A-Za-z0-9_]+)\b")
C_API_FROZEN_DECISIONS = frozenset(
    {
        "result-handle-is-the-bounded-chunk",
        "extended-single-statement-versus-simple-script",
        "callbacks-only-on-host-progress-or-dispatch",
        "backpressure-is-not-an-error-status",
        "copy-out-is-an-exact-byte-stream",
        "oversized-values-fail-at-a-hard-limit",
        "core-abi-freezes-after-runtime-events",
        "arrow-is-an-optional-adapter",
        "management-uses-an-operation-handle",
        "one-routed-instance-event-queue",
        "explicit-pre-v1-result-renumbering",
        "transaction-pin-status-and-telemetry-are-core",
        "copy-start-result-transfers-exclusive-ownership",
        "notices-are-request-scoped-and-routed",
        "scripts-use-distinct-options-and-text-results",
        "no-raw-protocol-or-c-transaction-callback-in-v1",
    }
)


class ApiContractError(RuntimeError):
    """The API declaration, policy, or upstream contract is incomplete."""


def validate_frozen_decisions(decisions: Any) -> int:
    if (
        not isinstance(decisions, list)
        or len(decisions) != len(set(decisions))
        or not C_API_FROZEN_DECISIONS <= set(decisions)
    ):
        raise ApiContractError(
            "the public API must preserve every frozen C API decision and "
            "record unique additive decisions"
        )
    return len(decisions)


def load_document(path: Path, expected_kind: str) -> dict[str, Any]:
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise ApiContractError(f"cannot read {path}: {exc}") from exc
    if not isinstance(document, dict):
        raise ApiContractError(f"{path} must contain a JSON object")
    if document.get("schema_version") != 1 or document.get("kind") != expected_kind:
        raise ApiContractError(f"{path} has an unsupported schema or kind")
    return document


def unique_entries(
    entries: Any, key: str, description: str
) -> dict[str, dict[str, Any]]:
    if not isinstance(entries, list):
        raise ApiContractError(f"{description} must be an array")
    result: dict[str, dict[str, Any]] = {}
    for entry in entries:
        if not isinstance(entry, dict) or not isinstance(entry.get(key), str):
            raise ApiContractError(f"invalid {description} entry")
        name = entry[key]
        if not name or name in result:
            raise ApiContractError(f"duplicate or empty {description}: {name!r}")
        result[name] = entry
    return result


def parse_consumer_output(content: str) -> dict[str, Any]:
    numerics: dict[str, int] = {}
    layouts: dict[str, dict[str, Any]] = {}
    for number, line in enumerate(content.splitlines(), start=1):
        fields = line.split()
        if not fields:
            continue
        if fields[0] == "numeric" and len(fields) == 3:
            name = fields[1]
            if name in numerics:
                raise ApiContractError(f"duplicate numeric output {name}")
            try:
                numerics[name] = int(fields[2])
            except ValueError as exc:
                raise ApiContractError(
                    f"invalid numeric output on line {number}"
                ) from exc
        elif fields[0] == "layout" and len(fields) == 4:
            name = fields[1]
            if name in layouts:
                raise ApiContractError(f"duplicate layout output {name}")
            try:
                layouts[name] = {
                    "size": int(fields[2]),
                    "alignment": int(fields[3]),
                    "fields": {},
                }
            except ValueError as exc:
                raise ApiContractError(
                    f"invalid layout output on line {number}"
                ) from exc
        elif fields[0] == "field" and len(fields) == 4:
            record_name, field_name = fields[1], fields[2]
            if record_name not in layouts:
                raise ApiContractError(
                    f"field output precedes layout for {record_name}"
                )
            record_fields = layouts[record_name]["fields"]
            if field_name in record_fields:
                raise ApiContractError(
                    f"duplicate field output {record_name}.{field_name}"
                )
            try:
                record_fields[field_name] = int(fields[3])
            except ValueError as exc:
                raise ApiContractError(
                    f"invalid field output on line {number}"
                ) from exc
        else:
            raise ApiContractError(f"unrecognized consumer output on line {number}")
    if not numerics or not layouts:
        raise ApiContractError("consumer output has no numeric or layout facts")
    return {"numerics": numerics, "layouts": layouts}


def flatten_numeric_registry(policy: dict[str, Any]) -> dict[str, int]:
    groups = policy.get("numeric_registry")
    if not isinstance(groups, list) or not groups:
        raise ApiContractError("numeric_registry must be a nonempty array")
    numerics: dict[str, int] = {}
    group_names: set[str] = set()
    for group in groups:
        if not isinstance(group, dict) or not isinstance(group.get("group"), str):
            raise ApiContractError("invalid numeric registry group")
        group_name = group["group"]
        if group_name in group_names:
            raise ApiContractError(f"duplicate numeric group {group_name}")
        group_names.add(group_name)
        values = group.get("values")
        if not isinstance(values, dict) or not values:
            raise ApiContractError(f"numeric group {group_name} is empty")
        seen_values: set[int] = set()
        for name, value in values.items():
            if not isinstance(name, str) or not isinstance(value, int) or value < 0:
                raise ApiContractError(f"invalid numeric value in {group_name}")
            if name in numerics:
                raise ApiContractError(f"duplicate numeric name {name}")
            if value in seen_values and group.get("allow_value_aliases") is not True:
                raise ApiContractError(
                    f"numeric group {group_name} reuses value {value}"
                )
            if group.get("bitmask") and (value == 0 or value & (value - 1)):
                raise ApiContractError(
                    f"numeric bit {name} is not a positive power of two"
                )
            numerics[name] = value
            seen_values.add(value)
    return numerics


def delivered_phase(policy: dict[str, Any]) -> str:
    abi = policy.get("abi")
    if not isinstance(abi, dict):
        raise ApiContractError("ABI policy is missing")
    phase = abi.get("delivered_through")
    if phase not in PHASE_RANK:
        raise ApiContractError(f"invalid delivered ABI phase {phase!r}")
    return phase


def compile_function_contracts(
    root: Path, policy: dict[str, Any], catalog: dict[str, Any]
) -> dict[str, Any]:
    profiles = policy.get("profiles")
    if not isinstance(profiles, dict) or not profiles:
        raise ApiContractError("profiles must be a nonempty object")
    owners = policy.get("implementation_owners")
    tests = policy.get("test_owners")
    if not isinstance(owners, dict) or not isinstance(tests, dict):
        raise ApiContractError("implementation and test owner registries are required")
    functions = unique_entries(policy.get("functions"), "name", "function contract")
    declarations = unique_entries(catalog.get("functions"), "name", "AST function")
    if set(functions) != set(declarations):
        raise ApiContractError(
            "public declaration/contract mismatch: "
            f"missing_contracts={sorted(set(declarations) - set(functions))}, "
            f"stale_contracts={sorted(set(functions) - set(declarations))}"
        )

    complete: list[dict[str, Any]] = []
    delivered = 0
    planned = 0
    current_phase = delivered_phase(policy)
    delivered_rank = PHASE_RANK[current_phase]
    for name in sorted(functions):
        entry = functions[name]
        profile_name = entry.get("profile")
        profile = profiles.get(profile_name)
        if not isinstance(profile_name, str) or not isinstance(profile, dict):
            raise ApiContractError(f"{name} has an unknown contract profile")
        contract = {**profile, **entry}
        missing = sorted(
            field
            for field in REQUIRED_CONTRACT_FIELDS
            if field not in contract or contract[field] in (None, "", [])
        )
        if missing:
            raise ApiContractError(f"{name} has incomplete semantics: {missing}")
        owner_name = contract.get("owner")
        test_name = contract.get("test")
        if owner_name not in owners:
            raise ApiContractError(f"{name} has unknown implementation owner")
        if test_name not in tests:
            raise ApiContractError(f"{name} has unknown test owner")
        phase = contract.get("phase")
        if phase not in PHASE_RANK:
            raise ApiContractError(f"{name} has unknown delivery phase {phase!r}")
        if PHASE_RANK[phase] <= delivered_rank:
            delivered += 1
            for label, relative in (
                ("implementation", owners[owner_name]),
                ("test", tests[test_name]),
            ):
                if not isinstance(relative, str) or not (root / relative).is_file():
                    raise ApiContractError(
                        f"delivered {name} has no existing {label} owner: {relative}"
                    )
        else:
            planned += 1
        complete.append(
            {
                "name": name,
                "header": declarations[name]["header"],
                "line": declarations[name]["line"],
                "function_type": declarations[name]["function_type"],
                "phase": phase,
                "profile": profile_name,
                "implementation_owner": owner_name,
                "implementation_path": owners[owner_name],
                "test_owner": test_name,
                "test_path": tests[test_name],
                "semantic_fields": sorted(REQUIRED_CONTRACT_FIELDS),
            }
        )
    return {
        "function_count": len(complete),
        "delivered_through": current_phase,
        "delivered_function_count": delivered,
        "planned_function_count": planned,
        "functions": complete,
    }


def validate_handles(policy: dict[str, Any], catalog: dict[str, Any]) -> dict[str, Any]:
    functions = {entry["name"] for entry in policy["functions"]}
    records = unique_entries(catalog.get("records"), "name", "AST record")
    handles = unique_entries(policy.get("handles"), "name", "handle")
    for name, handle in handles.items():
        record = records.get(name)
        if record is None or record.get("complete") is not False:
            raise ApiContractError(f"handle {name} is not an opaque public record")
        parent = handle.get("parent")
        if parent is not None and parent not in handles:
            raise ApiContractError(f"handle {name} has unknown parent {parent}")
        destructor = handle.get("destructor")
        if destructor not in functions:
            raise ApiContractError(f"handle {name} has no declared destructor")
        constructors = handle.get("constructors")
        if not isinstance(constructors, list) or not constructors:
            raise ApiContractError(f"handle {name} has no constructor")
        for constructor in constructors:
            if not isinstance(constructor, str) or (
                not constructor.startswith("internal:") and constructor not in functions
            ):
                raise ApiContractError(
                    f"handle {name} has unknown constructor {constructor!r}"
                )

    for name in handles:
        seen: set[str] = set()
        current: str | None = name
        while current is not None:
            if current in seen:
                raise ApiContractError(f"handle ownership cycle through {current}")
            seen.add(current)
            current = handles[current].get("parent")
    return {
        "handle_count": len(handles),
        "ownership_edges": sorted(
            [handle["parent"], name]
            for name, handle in handles.items()
            if handle.get("parent") is not None
        ),
        "owned_handles_without_destructor": 0,
        "ownership_cycles": 0,
    }


def validate_layouts(
    catalog: dict[str, Any], consumer: dict[str, Any]
) -> dict[str, Any]:
    ast_records = {
        entry["name"]: entry
        for entry in catalog.get("records", [])
        if entry.get("complete") is True
    }
    layouts = consumer["layouts"]
    if set(ast_records) != set(layouts):
        raise ApiContractError(
            "GCC/Clang public record mismatch: "
            f"missing_driver={sorted(set(ast_records) - set(layouts))}, "
            f"missing_ast={sorted(set(layouts) - set(ast_records))}"
        )
    compiled: list[dict[str, Any]] = []
    for name in sorted(ast_records):
        ast = ast_records[name]
        observed = layouts[name]
        ast_fields = {entry["name"]: entry["offset"] for entry in ast["fields"]}
        if ast["size"] != observed["size"] or ast["alignment"] != observed["alignment"]:
            raise ApiContractError(f"GCC/Clang size or alignment mismatch for {name}")
        if ast_fields != observed["fields"]:
            raise ApiContractError(f"GCC/Clang field offset mismatch for {name}")
        compiled.append(
            {
                "name": name,
                "size": ast["size"],
                "alignment": ast["alignment"],
                "fields": ast_fields,
            }
        )
    return {
        "record_count": len(compiled),
        "cross_compiler_layout_mismatches": 0,
        "records": compiled,
    }


def public_record_closure(
    function_names: set[str], catalog: dict[str, Any]
) -> tuple[set[str], set[str]]:
    functions = unique_entries(catalog.get("functions"), "name", "AST function")
    records = unique_entries(catalog.get("records"), "name", "AST record")
    pending: list[str] = []
    for name in function_names:
        function = functions.get(name)
        if function is None:
            raise ApiContractError(f"frozen function {name} is not declared")
        pending.extend(PUBLIC_RECORD.findall(function["function_type"]))

    complete: set[str] = set()
    opaque: set[str] = set()
    visited: set[str] = set()
    while pending:
        name = pending.pop()
        if name in visited:
            continue
        visited.add(name)
        record = records.get(name)
        if record is None:
            raise ApiContractError(f"public ABI references unknown record {name}")
        if record.get("complete") is False:
            opaque.add(name)
            continue
        if record.get("complete") is not True:
            raise ApiContractError(f"public record {name} has invalid completeness")
        complete.add(name)
        for field in record.get("fields", []):
            canonical_type = field.get("canonical_type")
            if not isinstance(canonical_type, str):
                raise ApiContractError(f"public record {name} has an invalid field")
            pending.extend(PUBLIC_RECORD.findall(canonical_type))
    return complete, opaque


def validate_abi_baseline(
    policy: dict[str, Any],
    catalog: dict[str, Any],
    numerics: dict[str, int],
    baseline: dict[str, Any],
) -> dict[str, Any]:
    if baseline.get("postgresql_major") != 19:
        raise ApiContractError("core ABI baseline must target PostgreSQL 19")
    policy_abi = policy.get("abi")
    baseline_abi = baseline.get("abi")
    if not isinstance(policy_abi, dict) or not isinstance(baseline_abi, dict):
        raise ApiContractError("core ABI metadata is missing")
    freeze_phase = baseline_abi.get("frozen_through")
    if (
        freeze_phase != policy_abi.get("core_freeze_after")
        or freeze_phase not in PHASE_RANK
    ):
        raise ApiContractError("core ABI freeze phase does not match policy")
    if policy_abi.get("state") != "frozen" or baseline_abi.get("state") != "frozen":
        raise ApiContractError("core ABI v1 must be frozen after runtime events")
    if baseline_abi.get("evolution") != "additive-within-major":
        raise ApiContractError("core ABI baseline has an invalid evolution policy")
    if baseline_abi.get("symbol_version") != "POSTGAMMA_1.0":
        raise ApiContractError("core ABI baseline has an invalid symbol version")

    baseline_major = baseline_abi.get("major")
    baseline_minor = baseline_abi.get("minor")
    current_major = policy_abi.get("major")
    current_minor = policy_abi.get("minor")
    if not all(
        isinstance(value, int)
        for value in (baseline_major, baseline_minor, current_major, current_minor)
    ):
        raise ApiContractError("core ABI version components must be integers")
    if current_major != baseline_major or current_minor < baseline_minor:
        raise ApiContractError("current ABI version predates or breaks the v1 baseline")
    encoded_baseline = (baseline_major << 16) | baseline_minor
    if baseline_abi.get("encoded_version") != encoded_baseline:
        raise ApiContractError("core ABI baseline has an invalid encoded version")
    same_minor = current_minor == baseline_minor

    policy_functions = unique_entries(
        policy.get("functions"), "name", "function contract"
    )
    catalog_functions = unique_entries(
        catalog.get("functions"), "name", "AST function"
    )
    baseline_functions = unique_entries(
        baseline.get("functions"), "name", "frozen function"
    )
    frozen_policy_functions = {
        name: entry
        for name, entry in policy_functions.items()
        if PHASE_RANK.get(entry.get("phase"), len(PHASE_RANK) + 1)
        <= PHASE_RANK[freeze_phase]
    }
    if set(baseline_functions) != set(frozen_policy_functions):
        raise ApiContractError(
            "frozen function inventory mismatch: "
            f"missing={sorted(set(frozen_policy_functions) - set(baseline_functions))}, "
            f"stale={sorted(set(baseline_functions) - set(frozen_policy_functions))}"
        )
    for name, frozen in baseline_functions.items():
        current = catalog_functions.get(name)
        if current is None:
            raise ApiContractError(f"frozen function {name} was removed")
        if frozen.get("phase") != frozen_policy_functions[name].get("phase"):
            raise ApiContractError(f"frozen function {name} changed delivery phase")
        if frozen.get("function_type") != current.get("function_type"):
            raise ApiContractError(f"frozen function signature changed: {name}")

    complete_closure, opaque_closure = public_record_closure(
        set(baseline_functions), catalog
    )
    baseline_opaque = baseline.get("opaque_records")
    if (
        not isinstance(baseline_opaque, list)
        or len(baseline_opaque) != len(set(baseline_opaque))
        or not all(isinstance(name, str) for name in baseline_opaque)
    ):
        raise ApiContractError("frozen opaque record inventory is invalid")
    if set(baseline_opaque) != opaque_closure:
        raise ApiContractError("frozen opaque record inventory changed")

    catalog_records = unique_entries(catalog.get("records"), "name", "AST record")
    baseline_records = unique_entries(
        baseline.get("records"), "name", "frozen record"
    )
    if not set(baseline_records) <= complete_closure:
        raise ApiContractError("frozen record inventory contains a stale record")
    if same_minor and set(baseline_records) != complete_closure:
        raise ApiContractError("ABI v1.0 record closure differs from its baseline")
    trailing_field_count = 0
    for name, frozen in baseline_records.items():
        current = catalog_records.get(name)
        if current is None or current.get("complete") is not True:
            raise ApiContractError(f"frozen record {name} is no longer complete")
        frozen_fields = frozen.get("fields")
        if not isinstance(frozen_fields, list) or not frozen_fields:
            raise ApiContractError(f"frozen record {name} has no fields")
        normalized_fields: list[dict[str, Any]] = []
        for field in frozen_fields:
            if not isinstance(field, dict):
                raise ApiContractError(f"frozen record {name} has an invalid field")
            normalized_fields.append(
                {
                    "name": field.get("name"),
                    "canonical_type": field.get("canonical_type"),
                    "offset": field.get("offset"),
                }
            )
        current_fields = [
            {
                "name": field.get("name"),
                "canonical_type": field.get("canonical_type"),
                "offset": field.get("offset"),
            }
            for field in current.get("fields", [])
        ]
        extensible = frozen.get("extensible") is True
        if extensible and normalized_fields[0]["name"] != "struct_size":
            raise ApiContractError(
                f"extensible frozen record {name} does not begin with struct_size"
            )
        if not extensible and frozen.get("extensible") is not False:
            raise ApiContractError(f"frozen record {name} has invalid extensibility")
        if current.get("alignment") != frozen.get("alignment"):
            raise ApiContractError(f"frozen record alignment changed: {name}")
        minimum_size = frozen.get("minimum_size")
        if not isinstance(minimum_size, int) or minimum_size <= 0:
            raise ApiContractError(f"frozen record {name} has an invalid size")
        if extensible:
            if current.get("size", 0) < minimum_size:
                raise ApiContractError(f"frozen record size shrank: {name}")
            if current_fields[: len(normalized_fields)] != normalized_fields:
                raise ApiContractError(f"frozen record prefix changed: {name}")
            trailing_field_count += len(current_fields) - len(normalized_fields)
            if same_minor and (
                current.get("size") != minimum_size
                or len(current_fields) != len(normalized_fields)
            ):
                raise ApiContractError(
                    f"record {name} changed without an ABI minor increment"
                )
        elif (
            current.get("size") != minimum_size
            or current_fields != normalized_fields
        ):
            raise ApiContractError(f"fixed frozen record changed: {name}")

    baseline_numerics = baseline.get("numeric_values")
    if not isinstance(baseline_numerics, dict) or not baseline_numerics:
        raise ApiContractError("frozen numeric baseline is missing")
    version_names = {"PGM_ABI_VERSION", "PGM_ABI_VERSION_MINOR"}
    for name, value in baseline_numerics.items():
        if not isinstance(name, str) or not isinstance(value, int):
            raise ApiContractError("frozen numeric baseline is invalid")
        if name not in version_names and numerics.get(name) != value:
            raise ApiContractError(f"frozen numeric value changed: {name}")
    expected_current_version = (current_major << 16) | current_minor
    if (
        numerics.get("PGM_ABI_VERSION_MAJOR") != current_major
        or numerics.get("PGM_ABI_VERSION_MINOR") != current_minor
        or numerics.get("PGM_ABI_VERSION") != expected_current_version
    ):
        raise ApiContractError("current ABI version numerics do not match policy")
    if same_minor and numerics != baseline_numerics:
        raise ApiContractError("ABI v1.0 numeric registry differs from its baseline")

    return {
        "status": "pass",
        "symbol_version": baseline_abi["symbol_version"],
        "frozen_through": freeze_phase,
        "frozen_function_count": len(baseline_functions),
        "frozen_numeric_count": len(baseline_numerics),
        "frozen_record_count": len(baseline_records),
        "frozen_opaque_record_count": len(baseline_opaque),
        "compatible_trailing_field_count": trailing_field_count,
        "breaking_changes": 0,
    }


def validate_state_models(
    policy: dict[str, Any], function_names: set[str]
) -> dict[str, Any]:
    models = unique_entries(policy.get("state_models"), "name", "state model")
    reports: list[dict[str, Any]] = []
    transition_count = 0
    for name, model in sorted(models.items()):
        states_raw = model.get("states")
        terminal_raw = model.get("terminal")
        initial = model.get("initial")
        transitions = model.get("transitions")
        if not isinstance(states_raw, list) or len(states_raw) != len(set(states_raw)):
            raise ApiContractError(f"state model {name} has invalid states")
        states = set(states_raw)
        if not isinstance(initial, str) or initial not in states:
            raise ApiContractError(f"state model {name} has invalid initial state")
        if not isinstance(terminal_raw, list) or not set(terminal_raw) <= states:
            raise ApiContractError(f"state model {name} has invalid terminal states")
        terminal = set(terminal_raw)
        if not isinstance(transitions, list) or not transitions:
            raise ApiContractError(f"state model {name} has no transitions")
        adjacency: dict[str, set[str]] = {state: set() for state in states}
        for transition in transitions:
            if not isinstance(transition, dict):
                raise ApiContractError(f"state model {name} has invalid transition")
            source = transition.get("from")
            target = transition.get("to")
            operation = transition.get("operation")
            if source not in states or target not in states:
                raise ApiContractError(f"state model {name} references unknown state")
            if not isinstance(operation, str) or (
                not operation.startswith("internal:") and operation not in function_names
            ):
                raise ApiContractError(
                    f"state model {name} references unknown operation {operation!r}"
                )
            if not transition.get("outcome"):
                raise ApiContractError(f"state model {name} has unclassified outcome")
            adjacency[source].add(target)
        for state in terminal:
            if adjacency[state]:
                raise ApiContractError(
                    f"state model {name} terminal state {state} has outgoing transitions"
                )
        reachable = {initial}
        queue = deque([initial])
        while queue:
            source = queue.popleft()
            for target in adjacency[source]:
                if target not in reachable:
                    reachable.add(target)
                    queue.append(target)
        if reachable != states:
            raise ApiContractError(
                f"state model {name} has unreachable states: {sorted(states - reachable)}"
            )
        transition_count += len(transitions)
        reports.append(
            {
                "name": name,
                "state_count": len(states),
                "transition_count": len(transitions),
                "reachable_state_count": len(reachable),
                "terminal_states": sorted(terminal),
            }
        )
    return {
        "model_count": len(reports),
        "transition_count": transition_count,
        "unreachable_state_count": 0,
        "models": reports,
    }


def enum_members(content: str, typedef_name: str, prefix: str) -> set[str]:
    match = re.search(
        rf"typedef\s+enum\s*\{{(?P<body>[^}}]*)\}}\s*{re.escape(typedef_name)}\s*;",
        content,
    )
    if match is None:
        raise ApiContractError(f"cannot find upstream enum {typedef_name}")
    return {token for token in IDENTIFIER.findall(match.group("body")) if token.startswith(prefix)}


def validate_protocol(
    root: Path,
    protocol: dict[str, Any],
    function_names: set[str],
    numerics: dict[str, int],
) -> dict[str, Any]:
    if protocol.get("postgresql_major") != 19:
        raise ApiContractError("protocol contract must target PostgreSQL 19")
    raw_files = protocol.get("upstream_files")
    if not isinstance(raw_files, dict) or set(raw_files) != {"libpq", "diagnostics"}:
        raise ApiContractError("protocol contract has invalid upstream files")
    contents: dict[str, str] = {}
    for name, relative in raw_files.items():
        if not isinstance(relative, str):
            raise ApiContractError("protocol source paths must be strings")
        path = (root / relative).resolve()
        try:
            path.relative_to(root.resolve())
            contents[name] = path.read_text(encoding="utf-8")
        except (OSError, ValueError) as exc:
            raise ApiContractError(f"cannot read protocol source {relative}") from exc
    all_upstream_tokens = set(IDENTIFIER.findall("\n".join(contents.values())))

    features = unique_entries(protocol.get("features"), "name", "protocol feature")
    dispositions = {"supported", "unsupported", "deferred"}
    for name, feature in features.items():
        if feature.get("disposition") not in dispositions:
            raise ApiContractError(f"feature {name} has invalid disposition")
        upstream = feature.get("upstream_symbols")
        public = feature.get("public_symbols")
        if not isinstance(upstream, list) or not upstream:
            raise ApiContractError(f"feature {name} has no upstream anchor")
        if not isinstance(public, list):
            raise ApiContractError(f"feature {name} has invalid public mapping")
        missing_upstream = sorted(set(upstream) - all_upstream_tokens)
        if missing_upstream:
            raise ApiContractError(
                f"feature {name} upstream anchors are stale: {missing_upstream}"
            )
        missing_public = sorted(set(public) - function_names)
        if missing_public:
            raise ApiContractError(
                f"feature {name} references unknown public functions: {missing_public}"
            )
        if feature["disposition"] == "supported" and not public:
            raise ApiContractError(f"supported feature {name} has no public API")
        if feature["disposition"] == "unsupported" and public:
            raise ApiContractError(f"unsupported feature {name} exposes public API")

    result_entries = unique_entries(
        protocol.get("result_statuses"), "upstream", "result status"
    )
    upstream_results = enum_members(contents["libpq"], "ExecStatusType", "PGRES_")
    if set(result_entries) != upstream_results:
        raise ApiContractError(
            "ExecStatusType coverage mismatch: "
            f"missing={sorted(upstream_results - set(result_entries))}, "
            f"stale={sorted(set(result_entries) - upstream_results)}"
        )
    for entry in result_entries.values():
        disposition = entry.get("disposition")
        public = entry.get("public")
        if disposition == "mapped" and public not in numerics:
            raise ApiContractError(f"result mapping has unknown public value {public}")
        if disposition == "unsupported" and public is not None:
            raise ApiContractError("unsupported result status must map to null")
        if disposition not in {"mapped", "unsupported"}:
            raise ApiContractError("result status has invalid disposition")

    transaction_entries = unique_entries(
        protocol.get("transaction_statuses"), "upstream", "transaction status"
    )
    upstream_transactions = enum_members(
        contents["libpq"], "PGTransactionStatusType", "PQTRANS_"
    )
    if set(transaction_entries) != upstream_transactions:
        raise ApiContractError("PGTransactionStatusType coverage mismatch")
    for entry in transaction_entries.values():
        if entry.get("public") not in numerics:
            raise ApiContractError("transaction status has unknown public mapping")

    diagnostic_entries = unique_entries(
        protocol.get("diagnostic_fields"), "upstream", "diagnostic field"
    )
    upstream_diagnostics = set(
        re.findall(r"^#define\s+(PG_DIAG_[A-Z_]+)\b", contents["diagnostics"], re.MULTILINE)
    )
    if set(diagnostic_entries) != upstream_diagnostics:
        raise ApiContractError(
            "PostgreSQL diagnostic coverage mismatch: "
            f"missing={sorted(upstream_diagnostics - set(diagnostic_entries))}, "
            f"stale={sorted(set(diagnostic_entries) - upstream_diagnostics)}"
        )
    for entry in diagnostic_entries.values():
        public = entry.get("public")
        if public is None:
            if entry.get("disposition") != "ignored" or not entry.get("reason"):
                raise ApiContractError("ignored diagnostic needs a rationale")
        elif public not in numerics:
            raise ApiContractError("diagnostic has unknown public mapping")

    return {
        "feature_count": len(features),
        "result_status_count": len(result_entries),
        "transaction_status_count": len(transaction_entries),
        "diagnostic_field_count": len(diagnostic_entries),
        "unclassified_upstream_results": 0,
        "unclassified_upstream_transactions": 0,
        "unclassified_upstream_diagnostics": 0,
    }


def validate_pre_v1_changes(
    policy: dict[str, Any], numerics: dict[str, int]
) -> dict[str, Any]:
    changes = policy.get("pre_v1_changes")
    if not isinstance(changes, list) or not changes:
        raise ApiContractError("the pre-v1 change register is missing")
    identifiers: set[str] = set()
    for change in changes:
        if not isinstance(change, dict) or not change.get("id") or not change.get("reason"):
            raise ApiContractError("invalid pre-v1 change record")
        removed = change.get("removed")
        replacement = change.get("replacement")
        if not isinstance(removed, dict) or not isinstance(replacement, dict):
            raise ApiContractError("pre-v1 change must record removed and replacement values")
        for name in removed:
            if name in numerics:
                raise ApiContractError(f"removed pre-v1 value is still active: {name}")
            identifiers.add(name)
        for name, value in replacement.items():
            if numerics.get(name) != value:
                raise ApiContractError(f"pre-v1 replacement is stale: {name}")
    return {"change_count": len(changes), "removed_identifiers": sorted(identifiers)}


def compile_report(
    root: Path,
    policy: dict[str, Any],
    catalog: dict[str, Any],
    protocol: dict[str, Any],
    baseline: dict[str, Any],
    c_output: str,
    cxx_output: str,
) -> dict[str, Any]:
    if policy.get("postgresql_major") != 19:
        raise ApiContractError("public API policy must target PostgreSQL 19")
    if c_output != cxx_output:
        raise ApiContractError("C11 and C++17 consumers disagree on public ABI facts")
    consumer = parse_consumer_output(c_output)
    numerics = flatten_numeric_registry(policy)
    if numerics != consumer["numerics"]:
        raise ApiContractError(
            "numeric registry/header mismatch: "
            f"missing={sorted(set(consumer['numerics']) - set(numerics))}, "
            f"stale={sorted(set(numerics) - set(consumer['numerics']))}"
        )
    abi = policy.get("abi")
    if not isinstance(abi, dict) or abi.get("encoding") != "major_shift_16_or_minor":
        raise ApiContractError("invalid ABI encoding policy")
    expected_abi = (abi.get("major", -1) << 16) | abi.get("minor", -1)
    if numerics.get("PGM_ABI_VERSION") != expected_abi:
        raise ApiContractError("packed ABI version does not match policy")
    declared_headers = set(policy.get("headers", []))
    catalog_headers = set(catalog.get("headers", []))
    if declared_headers != catalog_headers:
        raise ApiContractError("public header policy/catalog mismatch")

    function_contracts = compile_function_contracts(root, policy, catalog)
    function_names = {entry["name"] for entry in policy["functions"]}
    handles = validate_handles(policy, catalog)
    layouts = validate_layouts(catalog, consumer)
    states = validate_state_models(policy, function_names)
    protocol_report = validate_protocol(root, protocol, function_names, numerics)
    pre_v1 = validate_pre_v1_changes(policy, numerics)
    abi_baseline = validate_abi_baseline(policy, catalog, numerics, baseline)
    decisions = policy.get("frozen_decisions")
    decision_count = validate_frozen_decisions(decisions)

    core_frozen = abi.get("state") == "frozen" and abi_baseline["breaking_changes"] == 0
    report = {
        "schema_version": 1,
        "kind": "postgamma.c-api-closure",
        "postgresql_major": 19,
        "status": "pass",
        "abi": {
            "encoded_version": expected_abi,
            "major": abi["major"],
            "minor": abi["minor"],
            "state": abi["state"],
            "core_frozen": core_frozen,
        },
        "declarations": function_contracts,
        "ownership": handles,
        "numeric_registry": {
            "entry_count": len(numerics),
            "mismatches": 0,
            "values": dict(sorted(numerics.items())),
        },
        "layouts": layouts,
        "state_models": states,
        "postgresql_protocol": protocol_report,
        "pre_v1_changes": pre_v1,
        "core_abi_baseline": abi_baseline,
        "reviewed_decision_count": decision_count,
        "closure": {
            "declarations_without_contract": 0,
            "contracts_without_declaration": 0,
            "owned_handles_without_destructor": 0,
            "numeric_mismatches": 0,
            "layout_mismatches": 0,
            "unreachable_states": 0,
            "unclassified_upstream_semantics": 0,
        },
        "readiness": {
            "delivered_through": delivered_phase(policy),
            "core_abi_v1_frozen": core_frozen,
            "product_ready": False,
            "product_ready_reason": (
                "Core behavior and ABI evidence are delivered by executable product gates."
            ),
        },
    }
    return report


def write_json(path: Path, document: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        json.dumps(document, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", required=True, type=Path)
    parser.add_argument("--policy", required=True, type=Path)
    parser.add_argument("--catalog", required=True, type=Path)
    parser.add_argument("--protocol", required=True, type=Path)
    parser.add_argument("--baseline", required=True, type=Path)
    parser.add_argument("--c-output", required=True, type=Path)
    parser.add_argument("--cxx-output", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    try:
        root = args.root.resolve(strict=True)
        policy = load_document(args.policy.resolve(strict=True), POLICY_KIND)
        catalog = load_document(args.catalog.resolve(strict=True), CATALOG_KIND)
        protocol = load_document(args.protocol.resolve(strict=True), PROTOCOL_KIND)
        baseline = load_document(args.baseline.resolve(strict=True), BASELINE_KIND)
        c_output = args.c_output.resolve(strict=True).read_text(encoding="utf-8")
        cxx_output = args.cxx_output.resolve(strict=True).read_text(encoding="utf-8")
        report = compile_report(
            root, policy, catalog, protocol, baseline, c_output, cxx_output
        )
        write_json(args.output.resolve(), report)
    except (ApiContractError, OSError) as exc:
        parser.error(str(exc))
    print(
        "public C API contract: pass "
        f"({report['declarations']['function_count']} declarations, "
        f"{report['numeric_registry']['entry_count']} numeric values, "
        f"{report['state_models']['model_count']} executable models, "
        f"{report['core_abi_baseline']['frozen_function_count']} frozen functions)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
