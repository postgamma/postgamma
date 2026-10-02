#!/usr/bin/env python3
"""Validate logical-backend state mobility and carrier hygiene."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
from collections import Counter, defaultdict
from pathlib import Path
from typing import Any

from state_policy import (
    align_exact_candidates,
    catalog_candidates,
    load_json,
    policy_decisions,
    reject_unknown,
)


CATALOG_KIND = "postgamma.backend-mutable-state-catalog"
OWNERSHIP_KIND = "postgamma.backend-state-ownership"
MOBILITY_KIND = "postgamma.backend-state-mobility"
GUC_POLICY_KIND = "postgamma.guc-ownership"
OWNERS = ("immutable", "instance", "managed_guc", "role", "session")
SEMANTIC_OWNERS = ("immutable", "instance", "logical_backend", "session", "request")
MOBILITY_CLASSES = (
    "portable",
    "rebind_required",
    "pin_while_active",
    "carrier_only",
    "forbidden",
)
GUC_OWNERS = ("immutable", "instance", "role", "session")
GUC_SEMANTIC_OWNERS = {
    "immutable": "immutable",
    "instance": "instance",
    "role": "logical_backend",
    "session": "session",
}


class SessionMobilityError(ValueError):
    """The reviewed mobility contract does not match source-derived state."""


def stable_digest(value: Any) -> str:
    payload = json.dumps(value, sort_keys=True, separators=(",", ":"))
    return hashlib.sha256(payload.encode("utf-8")).hexdigest()


def _nonempty_string(value: Any, label: str) -> str:
    if not isinstance(value, str) or not value:
        raise SessionMobilityError(f"{label} must be a non-empty string")
    return value


def parse_mobility_policy(document: dict[str, Any]) -> dict[str, Any]:
    reject_unknown(
        document,
        (
            "schema_version",
            "kind",
            "review_fingerprint",
            "owner_defaults",
            "overrides",
            "runtime_carrier_fields",
        ),
        "mobility policy",
        SessionMobilityError,
    )
    if document.get("schema_version") != 1 or document.get("kind") != MOBILITY_KIND:
        raise SessionMobilityError(
            f"mobility policy: expected schema_version 1 and kind {MOBILITY_KIND}"
        )
    fingerprint = document.get("review_fingerprint")
    if not isinstance(fingerprint, dict) or set(fingerprint) != {
        "candidate_ids_sha256",
        "ownership_decisions_sha256",
    }:
        raise SessionMobilityError("mobility policy: invalid review_fingerprint")
    for name, value in fingerprint.items():
        if not isinstance(value, str) or re.fullmatch(r"[0-9a-f]{64}", value) is None:
            raise SessionMobilityError(f"mobility policy: invalid {name}")

    defaults = document.get("owner_defaults")
    if not isinstance(defaults, dict) or set(defaults) != set(OWNERS):
        raise SessionMobilityError(
            "mobility policy: owner_defaults must classify every ownership class"
        )
    parsed_defaults: dict[str, dict[str, str]] = {}
    for owner in OWNERS:
        value = defaults[owner]
        if not isinstance(value, dict):
            raise SessionMobilityError(f"mobility policy: default {owner} must be an object")
        reject_unknown(
            value,
            ("semantic_owner", "mobility", "rationale"),
            f"mobility policy: default {owner}",
            SessionMobilityError,
        )
        if value.get("semantic_owner") not in SEMANTIC_OWNERS:
            raise SessionMobilityError(
                f"mobility policy: default {owner} has invalid semantic_owner"
            )
        if value.get("mobility") not in MOBILITY_CLASSES:
            raise SessionMobilityError(
                f"mobility policy: default {owner} has invalid mobility"
            )
        _nonempty_string(value.get("rationale"), f"mobility policy: default {owner} rationale")
        parsed_defaults[owner] = dict(value)

    overrides_raw = document.get("overrides")
    if not isinstance(overrides_raw, list):
        raise SessionMobilityError("mobility policy: overrides must be an array")
    overrides: dict[str, dict[str, str]] = {}
    override_fields = {
        "id",
        "semantic_owner",
        "mobility",
        "rebind_hook",
        "quiescence",
        "rationale",
    }
    for index, value in enumerate(overrides_raw):
        label = f"mobility policy: overrides[{index}]"
        if not isinstance(value, dict):
            raise SessionMobilityError(f"{label} must be an object")
        reject_unknown(value, override_fields, label, SessionMobilityError)
        identifier = _nonempty_string(value.get("id"), f"{label} id")
        if identifier in overrides:
            raise SessionMobilityError(f"mobility policy: duplicate override {identifier}")
        if value.get("semantic_owner") not in SEMANTIC_OWNERS:
            raise SessionMobilityError(f"{label} has invalid semantic_owner")
        mobility = value.get("mobility")
        if mobility not in MOBILITY_CLASSES:
            raise SessionMobilityError(f"{label} has invalid mobility")
        _nonempty_string(value.get("rationale"), f"{label} rationale")
        if mobility in ("rebind_required", "pin_while_active", "carrier_only"):
            _nonempty_string(value.get("rebind_hook"), f"{label} rebind_hook")
            _nonempty_string(value.get("quiescence"), f"{label} quiescence")
        overrides[identifier] = dict(value)

    fields_raw = document.get("runtime_carrier_fields")
    if not isinstance(fields_raw, list) or not fields_raw:
        raise SessionMobilityError(
            "mobility policy: runtime_carrier_fields must be a non-empty array"
        )
    carrier_fields: dict[str, dict[str, str]] = {}
    for index, value in enumerate(fields_raw):
        label = f"mobility policy: runtime_carrier_fields[{index}]"
        if not isinstance(value, dict):
            raise SessionMobilityError(f"{label} must be an object")
        reject_unknown(
            value,
            ("name", "bind_hook", "unbind_hook", "rationale"),
            label,
            SessionMobilityError,
        )
        name = _nonempty_string(value.get("name"), f"{label} name")
        if name in carrier_fields:
            raise SessionMobilityError(f"mobility policy: duplicate carrier field {name}")
        for field in ("bind_hook", "unbind_hook", "rationale"):
            _nonempty_string(value.get(field), f"{label} {field}")
        carrier_fields[name] = dict(value)
    return {
        "fingerprint": dict(fingerprint),
        "defaults": parsed_defaults,
        "overrides": overrides,
        "carrier_fields": carrier_fields,
    }


def guc_semantic_owners(
    inventory: dict[str, Any], policy: dict[str, Any]
) -> dict[str, str]:
    if inventory.get("schema_version") != 1 or inventory.get("mode") != "scan":
        raise SessionMobilityError("GUC inventory: expected schema_version 1 scan output")
    if policy.get("schema_version") != 1 or policy.get("kind") != GUC_POLICY_KIND:
        raise SessionMobilityError(
            f"GUC policy: expected schema_version 1 and kind {GUC_POLICY_KIND}"
        )
    parameters = policy.get("parameters")
    declarations = inventory.get("declarations")
    if not isinstance(parameters, dict) or not isinstance(declarations, list):
        raise SessionMobilityError("GUC policy/inventory has an invalid parameter envelope")
    for name, owner in parameters.items():
        if not isinstance(name, str) or not name or owner not in GUC_OWNERS:
            raise SessionMobilityError(f"GUC policy: invalid ownership for {name!r}")

    by_usr: dict[str, set[str]] = defaultdict(set)
    for declaration in declarations:
        if not isinstance(declaration, dict) or declaration.get("definition") is not True:
            continue
        symbol_id = declaration.get("symbol_id")
        usr = declaration.get("usr")
        if not isinstance(symbol_id, str) or not symbol_id.startswith("pg.guc."):
            continue
        parameter = symbol_id[len("pg.guc.") :]
        if parameter.startswith("control."):
            continue
        if parameter not in parameters:
            raise SessionMobilityError(f"GUC inventory: {parameter} has no ownership policy")
        if not isinstance(usr, str) or not usr:
            raise SessionMobilityError(f"GUC inventory: {parameter} has an invalid USR")
        by_usr[usr].add(GUC_SEMANTIC_OWNERS[parameters[parameter]])
    conflicts = sorted(usr for usr, owners in by_usr.items() if len(owners) != 1)
    if conflicts:
        raise SessionMobilityError(
            "GUC inventory: storage has conflicting semantic owners: "
            + ", ".join(conflicts)
        )
    return {usr: next(iter(owners)) for usr, owners in by_usr.items()}


def validate_carrier_source(
    source_text: str, carrier_fields: dict[str, dict[str, str]]
) -> dict[str, Any]:
    match = re.search(
        r"typedef\s+struct\s+PostgammaCarrierFrame\s*\{(?P<body>.*?)\}\s*PostgammaCarrierFrame\s*;",
        source_text,
        re.DOTALL,
    )
    if match is None:
        raise SessionMobilityError("runtime source: PostgammaCarrierFrame is missing")
    body = match.group("body")
    missing = sorted(
        name
        for name in carrier_fields
        if re.search(rf"\b{re.escape(name)}\s*(?:\[[^]]+\])?\s*;", body) is None
    )
    if missing:
        raise SessionMobilityError(
            "runtime source: missing reviewed carrier field(s): " + ", ".join(missing)
        )
    hooks = sorted(
        {
            value[field]
            for value in carrier_fields.values()
            for field in ("bind_hook", "unbind_hook")
            if value[field] not in ("save_errno", "restore_errno", "clear_exit_jump_ready", "arm_outer_error_boundary")
        }
    )
    missing_hooks = [hook for hook in hooks if hook not in source_text]
    if missing_hooks:
        raise SessionMobilityError(
            "runtime source: missing carrier hook(s): " + ", ".join(missing_hooks)
        )
    return {
        "field_count": len(carrier_fields),
        "fields": [carrier_fields[name] for name in sorted(carrier_fields)],
        "source_sha256": hashlib.sha256(source_text.encode("utf-8")).hexdigest(),
    }


def validate_mobility(
    catalog: dict[str, Any],
    ownership: dict[str, Any],
    mobility_document: dict[str, Any],
    guc_inventory: dict[str, Any],
    guc_policy: dict[str, Any],
    runtime_source: str,
) -> dict[str, Any]:
    candidates = catalog_candidates(catalog, SessionMobilityError)
    decisions = policy_decisions(
        ownership,
        policy_kind=OWNERSHIP_KIND,
        owners=OWNERS,
        error_type=SessionMobilityError,
    )
    dormant = align_exact_candidates(candidates, decisions, SessionMobilityError)
    parsed = parse_mobility_policy(mobility_document)

    # The reviewed universe includes conditional candidates that may be dormant
    # under the current configure profile.  Exact catalog/policy alignment
    # above still rejects every unreviewed active candidate.
    candidate_digest = stable_digest(sorted(decisions))
    active_candidate_digest = stable_digest(sorted(candidates))
    ownership_digest = stable_digest(
        sorted(ownership["decisions"], key=lambda value: value["id"])
    )
    expected = parsed["fingerprint"]
    if expected["candidate_ids_sha256"] != candidate_digest:
        raise SessionMobilityError(
            "mobility policy: candidate set changed; review mobility before updating the fingerprint"
        )
    if expected["ownership_decisions_sha256"] != ownership_digest:
        raise SessionMobilityError(
            "mobility policy: ownership decisions changed; review mobility before updating the fingerprint"
        )

    overrides = parsed["overrides"]
    stale_overrides = sorted(set(overrides) - set(decisions))
    if stale_overrides:
        raise SessionMobilityError(
            "mobility policy: stale override(s): " + ", ".join(stale_overrides)
        )
    guc_owners = guc_semantic_owners(guc_inventory, guc_policy)
    records: list[dict[str, Any]] = []
    semantic_counts: Counter[str] = Counter()
    mobility_counts: Counter[str] = Counter()
    for identifier in sorted(candidates):
        candidate = candidates[identifier]
        decision = decisions[identifier]
        classification = dict(parsed["defaults"][decision["owner"]])
        if decision["owner"] == "managed_guc" and candidate["usr"] in guc_owners:
            classification["semantic_owner"] = guc_owners[candidate["usr"]]
            classification["rationale"] = (
                classification["rationale"]
                + " The reviewed GUC policy owns this parameter as "
                + classification["semantic_owner"]
                + "."
            )
        override = overrides.get(identifier)
        if override is not None:
            classification.update(
                {key: value for key, value in override.items() if key != "id"}
            )
        canonical_type = str(candidate.get("canonical_type", ""))
        if "jmp_buf" in canonical_type and classification["mobility"] not in (
            "carrier_only",
            "forbidden",
        ):
            raise SessionMobilityError(
                f"{identifier}: jump-buffer-backed state cannot be portable"
            )
        record = {
            "id": identifier,
            "symbol": candidate.get("name"),
            "usr": candidate.get("usr"),
            "source": {
                "path": candidate.get("definition_path"),
                "line": candidate.get("line"),
                "column": candidate.get("column"),
                "canonical_type": candidate.get("canonical_type"),
            },
            "owner": decision["owner"],
            "semantic_owner": classification["semantic_owner"],
            "mobility": classification["mobility"],
            "rebind_hook": classification.get("rebind_hook"),
            "quiescence": classification.get("quiescence"),
            "rationale": classification["rationale"],
            "manual_override": override is not None,
        }
        records.append(record)
        semantic_counts[record["semantic_owner"]] += 1
        mobility_counts[record["mobility"]] += 1

    return {
        "schema_version": 1,
        "kind": "postgamma.session-mobility-report",
        "candidate_count": len(candidates),
        "review_fingerprint": {
            "candidate_ids_sha256": candidate_digest,
            "active_candidate_ids_sha256": active_candidate_digest,
            "ownership_decisions_sha256": ownership_digest,
        },
        "semantic_owner_counts": {
            owner: semantic_counts[owner] for owner in SEMANTIC_OWNERS
        },
        "mobility_counts": {
            mobility: mobility_counts[mobility] for mobility in MOBILITY_CLASSES
        },
        "manual_override_count": len(overrides),
        "missing": [],
        "stale": [],
        "duplicate": [],
        "dormant_conditional_count": len(dormant),
        "dormant_conditional_decisions": dormant,
        "carrier": validate_carrier_source(runtime_source, parsed["carrier_fields"]),
        "records": records,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--catalog", required=True, type=Path)
    parser.add_argument("--ownership", required=True, type=Path)
    parser.add_argument("--mobility", required=True, type=Path)
    parser.add_argument("--guc-inventory", required=True, type=Path)
    parser.add_argument("--guc-policy", required=True, type=Path)
    parser.add_argument("--runtime-source", required=True, type=Path)
    parser.add_argument("--report", required=True, type=Path)
    args = parser.parse_args()
    try:
        runtime_source = args.runtime_source.read_text(encoding="utf-8")
        report = validate_mobility(
            load_json(args.catalog, SessionMobilityError),
            load_json(args.ownership, SessionMobilityError),
            load_json(args.mobility, SessionMobilityError),
            load_json(args.guc_inventory, SessionMobilityError),
            load_json(args.guc_policy, SessionMobilityError),
            runtime_source,
        )
    except (OSError, SessionMobilityError) as exc:
        parser.error(str(exc))
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(
        json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    counts = report["mobility_counts"]
    print(
        f"session mobility: {report['candidate_count']} exact classifications; "
        + ", ".join(f"{name}={counts[name]}" for name in MOBILITY_CLASSES)
        + f"; carrier_fields={report['carrier']['field_count']}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
