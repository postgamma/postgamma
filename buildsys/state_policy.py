#!/usr/bin/env python3
"""Domain-neutral helpers for source-derived state ownership policies."""

from __future__ import annotations

import json
from collections import Counter
from pathlib import Path
from typing import Any, Iterable


CATALOG_KIND = "postgamma.backend-mutable-state-catalog"
AVAILABILITIES = ("required", "conditional")


class StatePolicyError(ValueError):
    """A source-derived state catalog and its reviewed policy disagree."""


def _unique_object(
    pairs: list[tuple[str, Any]], error_type: type[ValueError]
) -> dict[str, Any]:
    counts = Counter(key for key, _value in pairs)
    duplicates = sorted(key for key, count in counts.items() if count > 1)
    if duplicates:
        raise error_type("duplicate JSON key(s): " + ", ".join(duplicates))
    return dict(pairs)


def load_json(
    path: Path, error_type: type[ValueError] = StatePolicyError
) -> dict[str, Any]:
    """Load one JSON object while rejecting duplicate keys."""

    try:
        value = json.loads(
            path.read_text(encoding="utf-8"),
            object_pairs_hook=lambda pairs: _unique_object(pairs, error_type),
        )
    except (OSError, json.JSONDecodeError, ValueError) as exc:
        if isinstance(exc, error_type) and str(exc).startswith("cannot read "):
            raise
        raise error_type(f"cannot read {path}: {exc}") from exc
    if not isinstance(value, dict):
        raise error_type(f"{path}: top-level value must be an object")
    return value


def reject_unknown(
    value: dict[str, Any],
    allowed: Iterable[str],
    label: str,
    error_type: type[ValueError] = StatePolicyError,
) -> None:
    unknown = sorted(set(value) - set(allowed))
    if unknown:
        raise error_type(f"{label} has unknown field(s): " + ", ".join(unknown))


def catalog_candidates(
    document: dict[str, Any],
    error_type: type[ValueError] = StatePolicyError,
) -> dict[str, dict[str, Any]]:
    """Validate the common AST inventory envelope and index its candidates."""

    if document.get("schema_version") != 1 or document.get("kind") != CATALOG_KIND:
        raise error_type(
            f"catalog: expected schema_version 1 and kind {CATALOG_KIND}"
        )
    raw = document.get("candidates")
    uses = document.get("uses")
    if not isinstance(raw, list) or not isinstance(uses, list):
        raise error_type("catalog: candidates and uses must be arrays")
    result: dict[str, dict[str, Any]] = {}
    for index, candidate in enumerate(raw):
        if not isinstance(candidate, dict):
            raise error_type(f"catalog: candidates[{index}] must be an object")
        identifier = candidate.get("id")
        usr = candidate.get("usr")
        if not isinstance(identifier, str) or not identifier:
            raise error_type(f"catalog: candidates[{index}] has invalid id")
        if not isinstance(usr, str) or not usr:
            raise error_type(f"catalog: {identifier} has invalid USR")
        if identifier in result:
            raise error_type(f"catalog: duplicate candidate {identifier}")
        result[identifier] = candidate
    summary = document.get("summary", {})
    if not isinstance(summary, dict) or summary.get("candidate_count") != len(result):
        raise error_type("catalog: candidate_count does not match candidates")
    return result


def policy_decisions(
    document: dict[str, Any],
    *,
    policy_kind: str,
    owners: Iterable[str],
    error_type: type[ValueError] = StatePolicyError,
    allowed_decision_fields: Iterable[str] = (
        "id",
        "owner",
        "rationale",
        "availability",
        "condition",
    ),
    allowed_top_fields: Iterable[str] = (
        "schema_version",
        "kind",
        "policy",
        "decisions",
    ),
) -> dict[str, dict[str, Any]]:
    """Parse the fields common to every reviewed ownership decision."""

    reject_unknown(document, allowed_top_fields, "policy", error_type)
    if document.get("schema_version") != 1 or document.get("kind") != policy_kind:
        raise error_type(
            f"policy: expected schema_version 1 and kind {policy_kind}"
        )
    raw = document.get("decisions")
    if not isinstance(raw, list):
        raise error_type("policy: decisions must be an array")
    owner_values = tuple(owners)
    result: dict[str, dict[str, Any]] = {}
    for index, decision in enumerate(raw):
        label = f"policy: decisions[{index}]"
        if not isinstance(decision, dict):
            raise error_type(f"{label} must be an object")
        reject_unknown(decision, allowed_decision_fields, label, error_type)
        identifier = decision.get("id")
        owner = decision.get("owner")
        rationale = decision.get("rationale")
        availability = decision.get("availability", "required")
        condition = decision.get("condition")
        if not isinstance(identifier, str) or not identifier:
            raise error_type(f"{label} has invalid id")
        if owner not in owner_values:
            raise error_type(f"policy: {identifier} has invalid owner {owner!r}")
        if not isinstance(rationale, str) or not rationale:
            raise error_type(f"policy: {identifier} requires a non-empty rationale")
        if availability not in AVAILABILITIES:
            raise error_type(
                f"policy: {identifier} has invalid availability {availability!r}"
            )
        if availability == "conditional" and (
            not isinstance(condition, str) or not condition
        ):
            raise error_type(
                f"policy: {identifier} requires a condition when availability is conditional"
            )
        if availability == "required" and condition is not None:
            raise error_type(
                f"policy: {identifier} has a condition but is not conditional"
            )
        if identifier in result:
            raise error_type(f"policy: duplicate decision {identifier}")
        parsed = dict(decision)
        parsed["availability"] = availability
        result[identifier] = parsed
    return result


def align_exact_candidates(
    candidates: dict[str, dict[str, Any]],
    decisions: dict[str, dict[str, Any]],
    error_type: type[ValueError] = StatePolicyError,
) -> list[str]:
    """Require every source candidate and every required policy row to align."""

    candidate_ids = set(candidates)
    decision_ids = set(decisions)
    missing = sorted(candidate_ids - decision_ids)
    stale = sorted(
        identifier
        for identifier in decision_ids - candidate_ids
        if decisions[identifier]["availability"] != "conditional"
    )
    dormant = sorted(
        identifier
        for identifier in decision_ids - candidate_ids
        if decisions[identifier]["availability"] == "conditional"
    )
    if missing or stale:
        problems = []
        if missing:
            problems.append("missing ownership decision(s): " + ", ".join(missing))
        if stale:
            problems.append("stale ownership decision(s): " + ", ".join(stale))
        raise error_type("; ".join(problems))
    return dormant
