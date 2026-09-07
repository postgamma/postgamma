#!/usr/bin/env python3
"""Build and verify the fail-closed embedded risk evidence report."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import tempfile
from collections import Counter
from pathlib import Path, PurePosixPath
from typing import Any

from postgresql_adapter import (
    adapter_sha256 as postgresql_adapter_sha256,
    load_adapter as load_postgresql_adapter,
)
from postgresql_embedded_adapter import (
    adapter_sha256 as embedded_adapter_sha256,
    load_adapter as load_embedded_adapter,
)


PROFILE_KIND = "postgamma.bootstrap-profile"
LEAF_KIND = "postgamma.bootstrap-leaf-evidence"
REPORT_KIND = "postgamma.bootstrap-evidence"
SCHEMA_VERSION = 1
CLAIM_STATUSES = frozenset({"not_run", "pass", "fail"})


class BootstrapEvidenceError(ValueError):
    """An bootstrap profile or evidence document is invalid."""


def _unique_object(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    counts = Counter(key for key, _value in pairs)
    duplicates = sorted(key for key, count in counts.items() if count > 1)
    if duplicates:
        raise BootstrapEvidenceError("duplicate JSON key(s): " + ", ".join(duplicates))
    return dict(pairs)


def load_json(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(
            path.read_text(encoding="utf-8"), object_pairs_hook=_unique_object
        )
    except (OSError, json.JSONDecodeError, BootstrapEvidenceError) as exc:
        raise BootstrapEvidenceError(f"cannot read {path}: {exc}") from exc
    if not isinstance(value, dict):
        raise BootstrapEvidenceError(f"{path}: top-level value must be an object")
    return value


def _reject_unknown(value: dict[str, Any], allowed: set[str], label: str) -> None:
    unknown = sorted(set(value) - allowed)
    if unknown:
        raise BootstrapEvidenceError(
            f"{label} has unknown field(s): " + ", ".join(unknown)
        )


def _required_string(value: dict[str, Any], field: str, label: str) -> str:
    result = value.get(field)
    if not isinstance(result, str) or not result:
        raise BootstrapEvidenceError(f"{label}.{field} must be a non-empty string")
    return result


def _relative_path(value: Any, label: str) -> str:
    if not isinstance(value, str) or not value:
        raise BootstrapEvidenceError(f"{label} must be a non-empty relative path")
    path = PurePosixPath(value)
    if path.is_absolute() or "." in path.parts or ".." in path.parts:
        raise BootstrapEvidenceError(f"{label} must not escape its evidence root")
    return value


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def validate_profile(document: dict[str, Any]) -> dict[str, Any]:
    _reject_unknown(
        document,
        {
            "schema_version",
            "kind",
            "id",
            "description",
            "claims",
            "supporting_evidence",
        },
        "profile",
    )
    if (
        document.get("schema_version") != SCHEMA_VERSION
        or document.get("kind") != PROFILE_KIND
    ):
        raise BootstrapEvidenceError(
            f"profile must use schema_version {SCHEMA_VERSION} and kind {PROFILE_KIND}"
        )
    _required_string(document, "id", "profile")
    _required_string(document, "description", "profile")
    raw_support = document.get("supporting_evidence", [])
    if not isinstance(raw_support, list):
        raise BootstrapEvidenceError("profile.supporting_evidence must be an array")
    supporting_evidence: list[dict[str, str]] = []
    support_ids: list[str] = []
    support_paths: list[str] = []
    for index, raw in enumerate(raw_support):
        label = f"profile.supporting_evidence[{index}]"
        if not isinstance(raw, dict):
            raise BootstrapEvidenceError(f"{label} must be an object")
        _reject_unknown(raw, {"id", "evidence", "description"}, label)
        identifier = _required_string(raw, "id", label)
        evidence = _relative_path(raw.get("evidence"), f"{label}.evidence")
        description = _required_string(raw, "description", label)
        support_ids.append(identifier)
        support_paths.append(evidence)
        supporting_evidence.append(
            {"id": identifier, "evidence": evidence, "description": description}
        )
    if len(support_ids) != len(set(support_ids)):
        raise BootstrapEvidenceError("profile contains duplicate supporting evidence IDs")
    if len(support_paths) != len(set(support_paths)):
        raise BootstrapEvidenceError("profile contains duplicate supporting evidence paths")
    raw_claims = document.get("claims")
    if not isinstance(raw_claims, list) or not raw_claims:
        raise BootstrapEvidenceError("profile.claims must be a non-empty array")
    claims: list[dict[str, Any]] = []
    identifiers: list[str] = []
    evidence_paths: list[str] = []
    for index, raw in enumerate(raw_claims):
        label = f"profile.claims[{index}]"
        if not isinstance(raw, dict):
            raise BootstrapEvidenceError(f"{label} must be an object")
        _reject_unknown(
            raw,
            {"id", "evidence", "required_for_completion", "description"},
            label,
        )
        identifier = _required_string(raw, "id", label)
        evidence = _relative_path(raw.get("evidence"), f"{label}.evidence")
        required = raw.get("required_for_completion")
        if not isinstance(required, bool):
            raise BootstrapEvidenceError(
                f"{label}.required_for_completion must be a boolean"
            )
        description = _required_string(raw, "description", label)
        identifiers.append(identifier)
        evidence_paths.append(evidence)
        claims.append(
            {
                "id": identifier,
                "evidence": evidence,
                "required_for_completion": required,
                "description": description,
            }
        )
    duplicate_ids = sorted(
        identifier
        for identifier, count in Counter(identifiers).items()
        if count > 1
    )
    if duplicate_ids:
        raise BootstrapEvidenceError(
            "profile contains duplicate claim id(s): " + ", ".join(duplicate_ids)
        )
    duplicate_paths = sorted(
        path for path, count in Counter(evidence_paths).items() if count > 1
    )
    if duplicate_paths:
        raise BootstrapEvidenceError(
            "profile contains duplicate evidence path(s): "
            + ", ".join(duplicate_paths)
        )
    if not any(claim["required_for_completion"] for claim in claims):
        raise BootstrapEvidenceError("profile has no completion claim")
    if set(support_paths).intersection(evidence_paths):
        raise BootstrapEvidenceError("claim and supporting evidence paths must be distinct")
    return {
        **document,
        "claims": claims,
        "supporting_evidence": supporting_evidence,
    }


def load_upstream_identity(path: Path) -> dict[str, Any]:
    document = load_json(path)
    _reject_unknown(
        document,
        {"schema_version", "repository", "path", "ref_name", "commit"},
        "upstream manifest",
    )
    if document.get("schema_version") != 1:
        raise BootstrapEvidenceError("upstream manifest must use schema_version 1")
    commit = _required_string(document, "commit", "upstream manifest")
    if len(commit) != 40 or any(character not in "0123456789abcdef" for character in commit):
        raise BootstrapEvidenceError("upstream manifest commit must be a lowercase SHA-1")
    return {
        "commit": commit,
        "ref_name": _required_string(document, "ref_name", "upstream manifest"),
        "manifest_sha256": sha256(path),
    }


def load_build_identity(
    configure_state: Path, config_log: Path, build_profile: str
) -> dict[str, Any]:
    document = load_json(configure_state)
    if document.get("schema_version") != 1:
        raise BootstrapEvidenceError("configure state must use schema_version 1")
    arguments = document.get("arguments")
    environment = document.get("environment")
    if not isinstance(arguments, list) or not all(
        isinstance(argument, str) for argument in arguments
    ):
        raise BootstrapEvidenceError("configure state arguments must be an argv array")
    if not isinstance(environment, dict) or not all(
        isinstance(name, str) and isinstance(value, str)
        for name, value in environment.items()
    ):
        raise BootstrapEvidenceError("configure state environment must be a string map")
    if not build_profile:
        raise BootstrapEvidenceError("build profile must be a non-empty string")
    return {
        "build_profile": build_profile,
        "configure_state_sha256": sha256(configure_state),
        "toolchain_config_log_sha256": sha256(config_log),
    }


def baseline_identity(
    upstream_manifest: Path,
    adapter_path: Path,
    embedded_adapter_path: Path,
    profile_path: Path,
    configure_state: Path,
    config_log: Path,
    build_profile: str,
) -> tuple[dict[str, Any], dict[str, Any]]:
    upstream = load_upstream_identity(upstream_manifest)
    build = load_build_identity(configure_state, config_log, build_profile)
    adapter = load_postgresql_adapter(adapter_path)
    embedded_adapter = load_embedded_adapter(embedded_adapter_path)
    profile = validate_profile(load_json(profile_path))
    majors = adapter["supported_postgresql_majors"]
    if majors != [19]:
        raise BootstrapEvidenceError(
            "embedded risk checks require a PostgreSQL 19 product adapter"
        )
    if embedded_adapter["product_postgresql_major"] != 19:
        raise BootstrapEvidenceError(
            "embedded risk checks require a PostgreSQL 19 embedded adapter"
        )
    return (
        {
            "postgresql_major": 19,
            "upstream_commit": upstream["commit"],
            "upstream_ref": upstream["ref_name"],
            "upstream_manifest_sha256": upstream["manifest_sha256"],
            "adapter_id": adapter["id"],
            "adapter_sha256": postgresql_adapter_sha256(adapter_path),
            "embedded_adapter_id": embedded_adapter["id"],
            "embedded_adapter_sha256": embedded_adapter_sha256(
                embedded_adapter_path
            ),
            "profile_id": profile["id"],
            "profile_sha256": sha256(profile_path),
            **build,
        },
        profile,
    )


def validate_leaf(
    document: dict[str, Any],
    claim_id: str,
    baseline: dict[str, Any],
    artifact_root: Path,
    label: str,
) -> dict[str, Any]:
    _reject_unknown(
        document,
        {
            "schema_version",
            "kind",
            "claim",
            "status",
            "baseline",
            "command",
            "artifacts",
            "limitations",
            "metrics",
        },
        label,
    )
    if (
        document.get("schema_version") != SCHEMA_VERSION
        or document.get("kind") != LEAF_KIND
    ):
        raise BootstrapEvidenceError(
            f"{label} must use schema_version {SCHEMA_VERSION} and kind {LEAF_KIND}"
        )
    if document.get("claim") != claim_id:
        raise BootstrapEvidenceError(
            f"{label}.claim is {document.get('claim')!r}, expected {claim_id!r}"
        )
    status = document.get("status")
    if status not in CLAIM_STATUSES:
        raise BootstrapEvidenceError(f"{label}.status is invalid: {status!r}")
    leaf_baseline = document.get("baseline")
    if not isinstance(leaf_baseline, dict):
        raise BootstrapEvidenceError(f"{label}.baseline must be an object")
    if leaf_baseline != baseline:
        raise BootstrapEvidenceError(f"{label}.baseline does not match the current build")
    command = document.get("command")
    if not isinstance(command, list) or not command or not all(
        isinstance(item, str) and item for item in command
    ):
        raise BootstrapEvidenceError(f"{label}.command must be a non-empty argv array")
    artifacts = document.get("artifacts", [])
    if not isinstance(artifacts, list):
        raise BootstrapEvidenceError(f"{label}.artifacts must be an array")
    if status == "pass" and not artifacts:
        raise BootstrapEvidenceError(f"{label}.artifacts must not be empty for pass evidence")
    artifact_paths: set[str] = set()
    artifact_root = artifact_root.resolve()
    for index, artifact in enumerate(artifacts):
        artifact_label = f"{label}.artifacts[{index}]"
        if not isinstance(artifact, dict) or set(artifact) != {"path", "sha256"}:
            raise BootstrapEvidenceError(
                f"{artifact_label} must contain only path and sha256"
            )
        relative = _relative_path(artifact["path"], f"{artifact_label}.path")
        digest = artifact["sha256"]
        if (
            not isinstance(digest, str)
            or len(digest) != 64
            or any(character not in "0123456789abcdef" for character in digest)
        ):
            raise BootstrapEvidenceError(
                f"{artifact_label}.sha256 must be a lowercase SHA-256"
            )
        if relative in artifact_paths:
            raise BootstrapEvidenceError(f"{label}.artifacts contains duplicate paths")
        artifact_paths.add(relative)
        path = (artifact_root / Path(*PurePosixPath(relative).parts)).resolve()
        try:
            path.relative_to(artifact_root)
        except ValueError as exc:
            raise BootstrapEvidenceError(
                f"{artifact_label}.path escapes the bootstrap build root"
            ) from exc
        if not path.is_file() or sha256(path) != digest:
            raise BootstrapEvidenceError(f"{artifact_label} is missing or stale")
    limitations = document.get("limitations", [])
    if not isinstance(limitations, list) or not all(
        isinstance(item, str) and item for item in limitations
    ):
        raise BootstrapEvidenceError(f"{label}.limitations must be an array of strings")
    metrics = document.get("metrics", {})
    if not isinstance(metrics, dict):
        raise BootstrapEvidenceError(f"{label}.metrics must be an object")
    return document


def collect_report(
    profile: dict[str, Any], baseline: dict[str, Any], evidence_dir: Path
) -> dict[str, Any]:
    claims: dict[str, str] = {}
    leaves: dict[str, dict[str, str]] = {}
    for claim in profile["claims"]:
        identifier = claim["id"]
        relative = claim["evidence"]
        leaf_path = evidence_dir / Path(*PurePosixPath(relative).parts)
        if not leaf_path.is_file():
            claims[identifier] = "not_run"
            continue
        leaf = validate_leaf(
            load_json(leaf_path),
            identifier,
            baseline,
            evidence_dir.parent,
            str(leaf_path),
        )
        claims[identifier] = leaf["status"]
        leaves[identifier] = {
            "path": relative,
            "sha256": sha256(leaf_path),
        }
    required = {
        claim["id"]
        for claim in profile["claims"]
        if claim["required_for_completion"]
    }
    support_status: dict[str, str] = {}
    support_facts: dict[str, dict[str, str]] = {}
    for support in profile["supporting_evidence"]:
        identifier = support["id"]
        relative = support["evidence"]
        support_path = evidence_dir / Path(*PurePosixPath(relative).parts)
        if not support_path.is_file():
            support_status[identifier] = "not_run"
            continue
        support_document = load_json(support_path)
        if (
            support_document.get("schema_version") != SCHEMA_VERSION
            or support_document.get("kind")
            != "postgamma.embedded-process-assumptions"
            or support_document.get("status") != "pass"
            or support_document.get("baseline") != baseline
            or support_document.get("product_ready") is not False
            or support_document.get("embedded_kernel_surface_frozen") is not True
            or not isinstance(support_document.get("records"), list)
            or not support_document["records"]
        ):
            raise BootstrapEvidenceError(
                f"supporting evidence {identifier} is stale or incomplete"
            )
        support_status[identifier] = "pass"
        support_facts[identifier] = {
            "path": relative,
            "sha256": sha256(support_path),
        }
    complete = all(claims[identifier] == "pass" for identifier in required) and all(
        status == "pass" for status in support_status.values()
    )
    return {
        "schema_version": SCHEMA_VERSION,
        "kind": REPORT_KIND,
        "baseline": baseline,
        "claims": dict(sorted(claims.items())),
        "leaf_evidence": dict(sorted(leaves.items())),
        "supporting_evidence": dict(sorted(support_facts.items())),
        "supporting_evidence_status": dict(sorted(support_status.items())),
        "product_ready": False,
        "bootstrap_complete": complete,
    }


def validate_report(
    report: dict[str, Any], profile: dict[str, Any], baseline: dict[str, Any], evidence_dir: Path
) -> dict[str, Any]:
    _reject_unknown(
        report,
        {
            "schema_version",
            "kind",
            "baseline",
            "claims",
            "leaf_evidence",
            "supporting_evidence",
            "supporting_evidence_status",
            "product_ready",
            "bootstrap_complete",
        },
        "bootstrap report",
    )
    if (
        report.get("schema_version") != SCHEMA_VERSION
        or report.get("kind") != REPORT_KIND
    ):
        raise BootstrapEvidenceError(
            f"bootstrap report must use schema_version {SCHEMA_VERSION} and kind {REPORT_KIND}"
        )
    if report.get("baseline") != baseline:
        raise BootstrapEvidenceError("bootstrap report baseline is stale")
    if report.get("product_ready") is not False:
        raise BootstrapEvidenceError(
            "risk-retirement evidence must never report product_ready=true"
        )
    expected = collect_report(profile, baseline, evidence_dir)
    if report.get("claims") != expected["claims"]:
        raise BootstrapEvidenceError("bootstrap report claim status does not match leaf evidence")
    if report.get("leaf_evidence") != expected["leaf_evidence"]:
        raise BootstrapEvidenceError("bootstrap report contains a stale leaf evidence hash")
    if report.get("supporting_evidence") != expected["supporting_evidence"]:
        raise BootstrapEvidenceError("bootstrap report contains stale supporting evidence")
    if report.get("supporting_evidence_status") != expected[
        "supporting_evidence_status"
    ]:
        raise BootstrapEvidenceError("bootstrap report supporting evidence status is inconsistent")
    if report.get("bootstrap_complete") is not expected["bootstrap_complete"]:
        raise BootstrapEvidenceError("bootstrap report completion status is inconsistent")
    return report


def write_json(path: Path, document: dict[str, Any]) -> None:
    content = json.dumps(document, indent=2, sort_keys=True) + "\n"
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.exists() and path.read_text(encoding="utf-8") == content:
        return
    with tempfile.NamedTemporaryFile(
        mode="w",
        encoding="utf-8",
        dir=path.parent,
        prefix=f".{path.name}.",
        delete=False,
    ) as handle:
        handle.write(content)
        temporary = Path(handle.name)
    try:
        os.replace(temporary, path)
    finally:
        temporary.unlink(missing_ok=True)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--profile", required=True, type=Path)
    parser.add_argument("--upstream-manifest", required=True, type=Path)
    parser.add_argument("--adapter", required=True, type=Path)
    parser.add_argument("--embedded-adapter", required=True, type=Path)
    parser.add_argument("--configure-state", required=True, type=Path)
    parser.add_argument("--config-log", required=True, type=Path)
    parser.add_argument("--build-profile", required=True)
    parser.add_argument("--evidence-dir", required=True, type=Path)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--output", type=Path)
    mode.add_argument("--verify-report", type=Path)
    parser.add_argument("--require-complete", action="store_true")
    args = parser.parse_args()
    try:
        baseline, profile = baseline_identity(
            args.upstream_manifest.resolve(),
            args.adapter.resolve(),
            args.embedded_adapter.resolve(),
            args.profile.resolve(),
            args.configure_state.resolve(),
            args.config_log.resolve(),
            args.build_profile,
        )
        evidence_dir = args.evidence_dir.resolve()
        if args.output is not None:
            report = collect_report(profile, baseline, evidence_dir)
            write_json(args.output.resolve(), report)
        else:
            assert args.verify_report is not None
            report = validate_report(
                load_json(args.verify_report.resolve()),
                profile,
                baseline,
                evidence_dir,
            )
        if args.require_complete and not report["bootstrap_complete"]:
            incomplete = [
                identifier
                for identifier, status in report["claims"].items()
                if status != "pass"
                and any(
                    claim["id"] == identifier
                    and claim["required_for_completion"]
                    for claim in profile["claims"]
                )
            ]
            incomplete.extend(
                f"support:{identifier}"
                for identifier, status in report[
                    "supporting_evidence_status"
                ].items()
                if status != "pass"
            )
            raise BootstrapEvidenceError(
                "required bootstrap claim(s) are incomplete: " + ", ".join(incomplete)
            )
    except (BootstrapEvidenceError, OSError, ValueError) as exc:
        parser.error(str(exc))
    print(
        "embedded risk evidence: "
        + ", ".join(
            f"{identifier}={status}"
            for identifier, status in report["claims"].items()
        )
        + f"; complete={str(report['bootstrap_complete']).lower()}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
