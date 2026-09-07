#!/usr/bin/env python3
"""Require exact alignment between PostgreSQL GUC facts and human ownership."""

from __future__ import annotations

import argparse
import json
from collections import Counter
from pathlib import Path
from typing import Any, Iterable


CATALOG_KIND = "postgamma.postgres-guc-catalog"
POLICY_KIND = "postgamma.guc-ownership"
ALLOWED_OWNERS = frozenset({"immutable", "instance", "role", "session"})


class PolicyError(RuntimeError):
    """A malformed or incomplete ownership decision."""


def unique_object(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    counts = Counter(key for key, _ in pairs)
    duplicates = sorted(key for key, count in counts.items() if count > 1)
    if duplicates:
        raise PolicyError("duplicate JSON key(s): " + ", ".join(duplicates))
    return dict(pairs)


def load_json(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(
            path.read_text(encoding="utf-8"), object_pairs_hook=unique_object
        )
    except (OSError, json.JSONDecodeError, PolicyError) as exc:
        raise PolicyError(f"cannot load {path}: {exc}") from exc
    if not isinstance(value, dict):
        raise PolicyError(f"{path}: root must be an object")
    return value


def require_document(document: dict[str, Any], kind: str, label: str) -> None:
    if document.get("schema_version") != 1 or document.get("kind") != kind:
        raise PolicyError(f"{label}: expected schema_version 1 and kind {kind}")


def catalog_names(document: dict[str, Any]) -> set[str]:
    require_document(document, CATALOG_KIND, "catalog")
    parameters = document.get("parameters")
    if not isinstance(parameters, list):
        raise PolicyError("catalog: parameters must be an array")
    names: list[str] = []
    for index, parameter in enumerate(parameters):
        if not isinstance(parameter, dict) or not isinstance(parameter.get("name"), str):
            raise PolicyError(f"catalog: parameters[{index}] has no string name")
        names.append(parameter["name"])
    duplicates = sorted(name for name, count in Counter(names).items() if count > 1)
    if duplicates:
        raise PolicyError("catalog: duplicate parameter(s): " + ", ".join(duplicates))
    declared_count = document.get("parameter_count")
    if declared_count != len(names):
        raise PolicyError(
            f"catalog: parameter_count is {declared_count}, discovered {len(names)}"
        )
    return set(names)


def policy_owners(document: dict[str, Any]) -> dict[str, str]:
    require_document(document, POLICY_KIND, "policy")
    parameters = document.get("parameters")
    if not isinstance(parameters, dict):
        raise PolicyError("policy: parameters must be an object")
    result: dict[str, str] = {}
    for name, owner in parameters.items():
        if not isinstance(owner, str) or owner not in ALLOWED_OWNERS:
            allowed = ", ".join(sorted(ALLOWED_OWNERS))
            raise PolicyError(f"policy: {name} has invalid owner {owner!r}; use {allowed}")
        result[name] = owner
    return result


def alignment_report(catalog: Iterable[str], policy: dict[str, str]) -> dict[str, Any]:
    catalog_set = set(catalog)
    policy_set = set(policy)
    counts = Counter(policy.values())
    return {
        "schema_version": 1,
        "kind": "postgamma.guc-ownership-alignment",
        "catalog_count": len(catalog_set),
        "policy_count": len(policy_set),
        "missing": sorted(catalog_set - policy_set),
        "stale": sorted(policy_set - catalog_set),
        "ownership_counts": {
            owner: counts.get(owner, 0) for owner in sorted(ALLOWED_OWNERS)
        },
    }


def validate_alignment(catalog: Iterable[str], policy: dict[str, str]) -> dict[str, Any]:
    report = alignment_report(catalog, policy)
    problems: list[str] = []
    if report["missing"]:
        problems.append(
            "missing ownership decision(s): " + ", ".join(report["missing"])
        )
    if report["stale"]:
        problems.append("stale ownership decision(s): " + ", ".join(report["stale"]))
    if problems:
        raise PolicyError("; ".join(problems))
    return report


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--catalog", required=True, type=Path)
    parser.add_argument("--policy", required=True, type=Path)
    parser.add_argument("--report", type=Path)
    args = parser.parse_args()
    try:
        catalog = catalog_names(load_json(args.catalog))
        policy = policy_owners(load_json(args.policy))
        report = validate_alignment(catalog, policy)
    except PolicyError as exc:
        parser.error(str(exc))
    if args.report:
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(
            json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8"
        )
    counts = report["ownership_counts"]
    summary = ", ".join(f"{owner}={counts[owner]}" for owner in sorted(counts))
    print(f"GUC ownership: {report['catalog_count']} aligned parameters ({summary})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
