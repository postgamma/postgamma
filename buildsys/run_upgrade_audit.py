#!/usr/bin/env python3
"""Build PostgreSQL fact catalogs for a candidate ref and audit the upgrade."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import subprocess
from pathlib import Path
from typing import Any, Sequence

from check_guc_policy import catalog_names, policy_owners
from postgresql_adapter import (
    AdapterError,
    inspect_generated_support_anchors,
    inspect_source_compatibility,
    load_adapter,
    validate_generated_support_anchors,
    validate_source_compatibility,
)
from upgrade_audit import (
    UpgradeAuditError,
    compile_report,
    load_json,
    print_summary,
    write_report,
)


class UpgradeRunError(RuntimeError):
    """The isolated candidate audit could not be prepared or executed."""


def command_output(arguments: Sequence[str], cwd: Path) -> str:
    result = subprocess.run(
        list(arguments),
        cwd=cwd,
        check=False,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    if result.returncode:
        detail = result.stderr.strip() or result.stdout.strip()
        raise UpgradeRunError(f"command failed ({result.returncode}): {detail}")
    return result.stdout.strip()


def run(arguments: Sequence[str], cwd: Path, environment: dict[str, str]) -> None:
    result = subprocess.run(list(arguments), cwd=cwd, env=environment, check=False)
    if result.returncode:
        raise UpgradeRunError(
            f"command failed with exit status {result.returncode}: "
            + " ".join(str(value) for value in arguments)
        )


def git(repository: Path, *arguments: str) -> str:
    return command_output(("git", "-C", str(repository), *arguments), repository)


def write_json_if_changed(path: Path, document: dict[str, Any]) -> None:
    content = json.dumps(document, indent=2, sort_keys=True) + "\n"
    if path.is_file() and path.read_text(encoding="utf-8") == content:
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content, encoding="utf-8")


def file_fingerprint(root: Path, path: Path) -> dict[str, str]:
    path = path.resolve()
    try:
        label = path.relative_to(root).as_posix()
    except ValueError:
        label = str(path)
    return {
        "path": label,
        "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
    }


def worktree_paths(porcelain: str) -> set[Path]:
    return {
        Path(field.removeprefix("worktree ")).resolve()
        for field in porcelain.split("\0")
        if field.startswith("worktree ")
    }


def ensure_candidate_worktree(
    repository: Path, commit: str, destination: Path
) -> None:
    if destination.exists():
        if not (destination / ".git").exists():
            raise UpgradeRunError(
                f"candidate worktree path exists but is not a Git worktree: {destination}"
            )
        actual = git(destination, "rev-parse", "HEAD")
        status = git(destination, "status", "--porcelain=v1", "--untracked-files=all")
        if actual != commit or status:
            raise UpgradeRunError(
                f"candidate worktree is not reusable: expected clean {commit}, "
                f"found {actual}{' with local changes' if status else ''}"
            )
        print(f"upgrade audit: reusing candidate worktree {destination}", flush=True)
        return
    registered = worktree_paths(git(repository, "worktree", "list", "--porcelain", "-z"))
    if destination.resolve() in registered:
        # `make clean` can remove the disposable directory before Git's
        # administrative record.  Remove only this exact stale registration.
        run(
            ("git", "-C", str(repository), "worktree", "remove", "--force", str(destination)),
            repository,
            dict(os.environ),
        )
    destination.parent.mkdir(parents=True, exist_ok=True)
    run(
        ("git", "-C", str(repository), "worktree", "add", "--detach", str(destination), commit),
        repository,
        dict(os.environ),
    )


def scan_policy(
    candidate_catalog: dict[str, Any], baseline_policy: dict[str, Any]
) -> dict[str, Any]:
    names = catalog_names(candidate_catalog)
    baseline = policy_owners(baseline_policy)
    return {
        "schema_version": 1,
        "kind": "postgamma.guc-ownership",
        "purpose": (
            "Ephemeral upgrade scan input only; new entries are placeholders and are "
            "not reviewed product ownership decisions."
        ),
        "parameters": {
            name: baseline.get(name, "session") for name in sorted(names)
        },
    }


def make_environment() -> dict[str, str]:
    environment = dict(os.environ)
    environment.pop("MAKEFLAGS", None)
    environment.pop("MFLAGS", None)
    return environment


def run_make(
    make: str,
    root: Path,
    jobs: int,
    assignments: dict[str, Path | str],
    targets: Sequence[str],
) -> None:
    arguments = [
        make,
        "--silent",
        "--no-print-directory",
        "-C",
        str(root),
        f"JOBS={jobs}",
    ]
    arguments.extend(f"{name}={value}" for name, value in assignments.items())
    arguments.extend(targets)
    run(arguments, root, make_environment())


def upstream_diff(repository: Path, baseline: str, candidate: str) -> dict[str, Any]:
    numstat = git(repository, "diff", "--numstat", f"{baseline}..{candidate}")
    files = 0
    insertions = 0
    deletions = 0
    binary_files = 0
    for line in numstat.splitlines():
        if not line:
            continue
        added, removed, _path = line.split("\t", 2)
        files += 1
        if added == "-" or removed == "-":
            binary_files += 1
        else:
            insertions += int(added)
            deletions += int(removed)
    return {
        "file_count": files,
        "insertions": insertions,
        "deletions": deletions,
        "binary_file_count": binary_files,
        "commits_ahead": int(git(repository, "rev-list", "--count", f"{baseline}..{candidate}")),
        "commits_behind": int(git(repository, "rev-list", "--count", f"{candidate}..{baseline}")),
    }


def engine_fingerprint(root: Path) -> dict[str, Any]:
    owned_paths = [root / "Makefile"]
    for directory in (root / "buildsys", root / "compiler", root / "runtime"):
        owned_paths.extend(
            path
            for path in directory.rglob("*")
            if path.is_file()
            and "__pycache__" not in path.parts
            and path.suffix not in {".pyc", ".swp"}
        )
    digest = hashlib.sha256()
    for path in sorted(set(owned_paths)):
        relative = path.relative_to(root).as_posix()
        digest.update(relative.encode("utf-8") + b"\0")
        digest.update(path.read_bytes())
        digest.update(b"\0")
    status = command_output(
        (
            "git",
            "status",
            "--porcelain=v1",
            "--untracked-files=all",
            "--",
            "Makefile",
            "buildsys",
            "compiler",
            "runtime",
        ),
        root,
    )
    return {
        "shared_implementation": True,
        "project_commit": command_output(("git", "rev-parse", "HEAD"), root),
        "owned_source_sha256": digest.hexdigest(),
        "owned_source_dirty": bool(status),
    }


def runtime_hook_diagnostics(
    adapter: dict[str, Any], inventory: dict[str, Any]
) -> list[dict[str, Any]]:
    """Normalize compiler injection diagnostics for an upgrade report."""
    diagnostics = inventory.get("injection_diagnostics")
    if not isinstance(diagnostics, list):
        raise UpgradeRunError("AST inventory has no runtime-hook diagnostics")
    hooks = {hook["id"]: hook for hook in adapter["runtime_hooks"]}
    result: list[dict[str, Any]] = []
    seen: set[str] = set()
    for entry in diagnostics:
        if not isinstance(entry, dict) or entry.get("id") not in hooks:
            raise UpgradeRunError("AST inventory has unknown runtime-hook diagnostics")
        identifier = entry["id"]
        if identifier in seen:
            raise UpgradeRunError(
                f"AST inventory has duplicate runtime-hook diagnostic {identifier}"
            )
        seen.add(identifier)
        hook = hooks[identifier]
        expected = entry.get("expected_matches")
        actual = entry.get("actual_matches")
        errors = entry.get("errors")
        status = entry.get("status")
        if expected != hook["expected_matches"]:
            raise UpgradeRunError(
                f"AST inventory runtime-hook {identifier} expected_matches drifted"
            )
        if not isinstance(actual, int) or isinstance(actual, bool) or actual < 0:
            raise UpgradeRunError(
                f"AST inventory runtime-hook {identifier} has invalid actual_matches"
            )
        if (
            not isinstance(errors, list)
            or any(not isinstance(message, str) or not message for message in errors)
            or len(errors) != len(set(errors))
        ):
            raise UpgradeRunError(
                f"AST inventory runtime-hook {identifier} has invalid errors"
            )
        expected_status = (
            "ok" if actual == hook["expected_matches"] and not errors else "incompatible"
        )
        if status != expected_status:
            raise UpgradeRunError(
                f"AST inventory runtime-hook {identifier} has inconsistent status"
            )
        result.append(
            {
                "id": identifier,
                "class": "runtime_hook",
                "severity": "error",
                "status": status,
                "expected_matches": expected,
                "actual_matches": actual,
                "errors": errors,
                "source_file_suffixes": hook.get("source_file_suffixes", []),
            }
        )
    if set(hooks) != seen:
        raise UpgradeRunError("AST inventory runtime-hook diagnostics are incomplete")
    return sorted(result, key=lambda entry: entry["id"])


def execution_assumption_diagnostics(
    adapter: dict[str, Any], inventory: dict[str, Any]
) -> list[dict[str, Any]]:
    """Normalize fail-loud execution-model assumption diagnostics."""
    diagnostics = inventory.get("assumption_diagnostics")
    if not isinstance(diagnostics, list):
        raise UpgradeRunError("AST inventory has no execution-assumption diagnostics")
    assumptions = {
        assumption["id"]: assumption
        for assumption in adapter["execution_model_assumptions"]
    }
    result: list[dict[str, Any]] = []
    seen: set[str] = set()
    for entry in diagnostics:
        if not isinstance(entry, dict) or entry.get("id") not in assumptions:
            raise UpgradeRunError("AST inventory has unknown execution assumption")
        identifier = entry["id"]
        if identifier in seen:
            raise UpgradeRunError(
                f"AST inventory has duplicate execution assumption {identifier}"
            )
        seen.add(identifier)
        assumption = assumptions[identifier]
        expected = entry.get("expected_matches")
        actual = entry.get("actual_matches")
        status = entry.get("status")
        matches = entry.get("matches")
        if expected != assumption["expected_matches"]:
            raise UpgradeRunError(
                f"AST inventory execution assumption {identifier} expected_matches drifted"
            )
        if not isinstance(actual, int) or isinstance(actual, bool) or actual < 0:
            raise UpgradeRunError(
                f"AST inventory execution assumption {identifier} has invalid actual_matches"
            )
        if not isinstance(matches, list) or len(matches) != actual:
            raise UpgradeRunError(
                f"AST inventory execution assumption {identifier} has invalid matches"
            )
        sources_allowed = all(
            isinstance(match, dict) and match.get("allowed") is True
            for match in matches
        )
        expected_status = (
            "ok" if actual == assumption["expected_matches"] and sources_allowed
            else "incompatible"
        )
        if status != expected_status:
            raise UpgradeRunError(
                f"AST inventory execution assumption {identifier} has inconsistent status"
            )
        result.append(
            {
                "id": identifier,
                "class": "execution_assumption",
                "severity": "error",
                "status": status,
                "callee": assumption["callee"],
                "expected_matches": expected,
                "actual_matches": actual,
                "matches": matches,
                "allowed_source_file_suffixes": assumption[
                    "allowed_source_file_suffixes"
                ],
            }
        )
    if set(assumptions) != seen:
        raise UpgradeRunError("AST inventory execution-assumption diagnostics are incomplete")
    return sorted(result, key=lambda entry: entry["id"])


def run_audit(args: argparse.Namespace) -> Path:
    root = args.project_root.resolve()
    repository = args.postgres_source.resolve()
    baseline_build = args.baseline_build.resolve()
    upstream_manifest_path = args.upstream_manifest.resolve()
    baseline_adapter_path = args.adapter.resolve()
    candidate_adapter_path = (
        args.candidate_adapter.resolve()
        if args.candidate_adapter is not None
        else baseline_adapter_path
    )
    baseline_upstream = load_json(upstream_manifest_path)
    baseline_commit = git(repository, "rev-parse", "HEAD")
    if baseline_commit != baseline_upstream.get("commit"):
        raise UpgradeRunError(
            f"baseline PostgreSQL is {baseline_commit}, manifest expects "
            f"{baseline_upstream.get('commit')}"
        )
    candidate_commit = git(repository, "rev-parse", "--verify", f"{args.ref}^{{commit}}")
    short_commit = candidate_commit[:12]
    audit_root = baseline_build / "upgrade-audit"
    candidate_root = audit_root / "candidates" / short_commit
    candidate_build = candidate_root / "build"
    worktree = audit_root / "worktrees" / f"postgres-{short_commit}"
    ensure_candidate_worktree(repository, candidate_commit, worktree)

    baseline_adapter = load_adapter(baseline_adapter_path)
    candidate_adapter = load_adapter(candidate_adapter_path)
    baseline_major = validate_source_compatibility(baseline_adapter, repository)
    candidate_major, _candidate_declared = inspect_source_compatibility(
        candidate_adapter, worktree
    )
    baseline_anchors = validate_generated_support_anchors(
        baseline_adapter, repository
    )
    candidate_anchors = inspect_generated_support_anchors(
        candidate_adapter, worktree
    )

    candidate_manifest_path = candidate_root / "input/upstream.json"
    candidate_manifest = {
        "schema_version": 1,
        "repository": baseline_upstream["repository"],
        "path": "postgres",
        "ref_name": args.ref,
        "commit": candidate_commit,
    }
    write_json_if_changed(candidate_manifest_path, candidate_manifest)

    common_assignments: dict[str, Path | str] = {
        "BUILD_DIR": baseline_build,
        "PG_SOURCE": repository,
        "UPSTREAM_MANIFEST": upstream_manifest_path,
        "POSTGRES_ADAPTER": baseline_adapter_path,
        "GUC_OWNERSHIP": args.guc_policy.resolve(),
        "BACKEND_STATE_OWNERSHIP": args.state_policy.resolve(),
        "BACKEND_STATE_RULES": args.state_rules.resolve(),
    }
    print("upgrade audit: deriving baseline catalogs", flush=True)
    run_make(
        args.make,
        root,
        args.jobs,
        common_assignments,
        ("backend-state-policy-check", "backend-inheritance-catalog"),
    )

    candidate_assignments: dict[str, Path | str] = {
        "BUILD_DIR": candidate_build,
        "TOOL_BUILD": baseline_build / "tools",
        "PG_SOURCE": worktree,
        "UPSTREAM_MANIFEST": candidate_manifest_path,
        "POSTGRES_ADAPTER": candidate_adapter_path,
        "POSTGRES_CANDIDATE_PROBE": "1",
    }
    print("upgrade audit: deriving candidate GUC catalog", flush=True)
    run_make(
        args.make,
        root,
        args.jobs,
        candidate_assignments,
        ("guc-catalog",),
    )
    candidate_guc_catalog_path = candidate_build / "catalog/gucs.json"
    scan_policy_path = candidate_root / "input/gucs-scan-policy.json"
    write_json_if_changed(
        scan_policy_path,
        scan_policy(
            load_json(candidate_guc_catalog_path),
            load_json(args.guc_policy.resolve()),
        ),
    )
    candidate_assignments["GUC_OWNERSHIP"] = scan_policy_path
    print("upgrade audit: deriving candidate AST catalogs", flush=True)
    run_make(
        args.make,
        root,
        args.jobs,
        candidate_assignments,
        ("state-inventory", "ast-scan", "backend-inheritance-catalog"),
    )

    output = (
        args.output.resolve()
        if args.output is not None
        else baseline_build / "reports" / f"upgrade-audit-{short_commit}.json"
    )
    baseline_inventory = load_json(baseline_build / "inventory/gucs.json")
    candidate_inventory = load_json(candidate_build / "inventory/gucs.json")
    engine = engine_fingerprint(root)
    engine["baseline_generated_support_anchors"] = baseline_anchors
    engine["candidate_generated_support_anchors"] = candidate_anchors
    engine["baseline_runtime_hooks"] = runtime_hook_diagnostics(
        baseline_adapter, baseline_inventory
    )
    engine["candidate_runtime_hooks"] = runtime_hook_diagnostics(
        candidate_adapter, candidate_inventory
    )
    engine["baseline_execution_assumptions"] = execution_assumption_diagnostics(
        baseline_adapter, baseline_inventory
    )
    engine["candidate_execution_assumptions"] = execution_assumption_diagnostics(
        candidate_adapter, candidate_inventory
    )
    report = compile_report(
        baseline_upstream=baseline_upstream,
        candidate_upstream=candidate_manifest,
        baseline_major=baseline_major,
        candidate_major=candidate_major,
        baseline_guc_catalog=load_json(baseline_build / "catalog/gucs.json"),
        candidate_guc_catalog=load_json(candidate_guc_catalog_path),
        baseline_state_catalog=load_json(baseline_build / "catalog/backend-state.json"),
        candidate_state_catalog=load_json(candidate_build / "catalog/backend-state.json"),
        baseline_guc_inventory=baseline_inventory,
        candidate_guc_inventory=candidate_inventory,
        baseline_state_policy=load_json(args.state_policy.resolve()),
        baseline_inheritance_catalog=load_json(
            baseline_build / "catalog/backend-inheritance.json"
        ),
        candidate_inheritance_catalog=load_json(
            candidate_build / "catalog/backend-inheritance.json"
        ),
        baseline_adapter_path=baseline_adapter_path,
        candidate_adapter_path=candidate_adapter_path,
        upstream_diff=upstream_diff(repository, baseline_commit, candidate_commit),
        engine=engine,
        inputs={
            "configure": {
                name: os.environ.get(name, "")
                for name in (
                    "PG_CONFIGURE_ARGS",
                    "PG_REFERENCE_CONFIGURE_ARGS",
                    "PG_GENERATED_CONFIGURE_ARGS",
                )
            },
            "guc_ownership": file_fingerprint(root, args.guc_policy),
            "backend_state_ownership": file_fingerprint(root, args.state_policy),
            "backend_state_review_rules": file_fingerprint(root, args.state_rules),
            "candidate_scan_policy": file_fingerprint(root, scan_policy_path),
        },
    )
    write_report(report, output)
    print_summary(report, output)
    return output


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--project-root", required=True, type=Path)
    parser.add_argument("--postgres-source", required=True, type=Path)
    parser.add_argument("--upstream-manifest", required=True, type=Path)
    parser.add_argument("--adapter", required=True, type=Path)
    parser.add_argument("--candidate-adapter", type=Path)
    parser.add_argument("--guc-policy", required=True, type=Path)
    parser.add_argument("--state-policy", required=True, type=Path)
    parser.add_argument("--state-rules", required=True, type=Path)
    parser.add_argument("--baseline-build", required=True, type=Path)
    parser.add_argument("--ref", required=True)
    parser.add_argument("--make", default="make")
    parser.add_argument("--jobs", type=int, default=1)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    if args.jobs <= 0:
        parser.error("--jobs must be positive")
    if not args.ref:
        parser.error("--ref is required; for example --ref origin/master")
    try:
        run_audit(args)
    except (AdapterError, UpgradeAuditError, UpgradeRunError, OSError) as exc:
        parser.error(str(exc))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
