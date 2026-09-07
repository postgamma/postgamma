#!/usr/bin/env python3
"""Build a fixed PostgreSQL candidate as pristine and threaded trees."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import subprocess
import sys
import time
from pathlib import Path
from typing import Any, Sequence

from check_backend_state_policy import load_json
from postgresql_adapter import inspect_source_compatibility, load_adapter
from propose_backend_state_policy import propose
from run_upgrade_audit import (
    command_output,
    ensure_candidate_worktree,
    git,
    scan_policy,
    write_json_if_changed,
)


REPORT_KIND = "postgamma.upstream-canary"
PROFILES = ("daily", "weekly")


class CanaryError(RuntimeError):
    """The upstream canary could not prepare a reproducible candidate."""


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def source_digest(root: Path) -> str:
    digest = hashlib.sha256()
    inputs = [root / "Makefile"]
    for directory in (root / "buildsys", root / "compiler/src", root / "runtime"):
        inputs.extend(path for path in directory.rglob("*") if path.is_file())
    for path in sorted(inputs):
        if path.name == "Makefile" or path.suffix in (".c", ".cc", ".cpp", ".h", ".py"):
            digest.update(path.relative_to(root).as_posix().encode("utf-8"))
            digest.update(b"\0")
            digest.update(path.read_bytes())
            digest.update(b"\0")
    return digest.hexdigest()


def tool_identity(arguments: Sequence[str], cwd: Path) -> str:
    result = subprocess.run(
        list(arguments),
        cwd=cwd,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
    )
    first_line = result.stdout.splitlines()[0] if result.stdout else "no version output"
    return f"exit={result.returncode} {first_line}"


def candidate_inheritance_policy(
    catalog: dict[str, Any], baseline_policy: dict[str, Any]
) -> dict[str, Any]:
    """Create an explicitly unreviewed policy so a probe can reach compilation."""
    states = catalog.get("states")
    baseline_states = baseline_policy.get("states")
    if not isinstance(states, list) or not isinstance(baseline_states, list):
        raise CanaryError("candidate inheritance catalog or baseline policy is invalid")
    existing = {
        state["id"]: state
        for state in baseline_states
        if isinstance(state, dict) and isinstance(state.get("id"), str)
    }
    decisions: list[dict[str, Any]] = []
    candidate_ids: set[str] = set()
    for state in states:
        identifier = state.get("id") if isinstance(state, dict) else None
        if not isinstance(identifier, str) or not identifier:
            raise CanaryError("candidate inheritance catalog contains an invalid state")
        candidate_ids.add(identifier)
        if identifier in existing:
            decisions.append(dict(existing[identifier]))
        else:
            decisions.append(
                {
                    "id": identifier,
                    "strategy": "copy_value",
                    "rationale": (
                        "unreviewed candidate-probe placeholder; compilation evidence only"
                    ),
                }
            )
    for identifier, state in existing.items():
        if identifier not in candidate_ids and state.get("availability") == "conditional":
            decisions.append(dict(state))
    decisions.sort(key=lambda state: state["id"])
    return {
        "schema_version": 1,
        "kind": "postgamma.backend-inheritance-policy",
        "purpose": (
            "Ephemeral upstream-canary policy. New entries are unreviewed "
            "copy-value placeholders and never become product policy automatically."
        ),
        "guc_transfer": baseline_policy["guc_transfer"],
        "startup_data_transfer": baseline_policy["startup_data_transfer"],
        "client_socket_transfer": baseline_policy["client_socket_transfer"],
        "states": decisions,
    }


def make_arguments(
    make: str,
    root: Path,
    jobs: int,
    assignments: dict[str, Path | str],
    targets: Sequence[str],
) -> list[str]:
    arguments = [
        make,
        "--no-print-directory",
        "-C",
        str(root),
        f"JOBS={jobs}",
    ]
    arguments.extend(f"{name}={value}" for name, value in assignments.items())
    arguments.extend(targets)
    return arguments


def run_stage(
    identifier: str,
    arguments: Sequence[str],
    log_dir: Path,
) -> dict[str, Any]:
    log_dir.mkdir(parents=True, exist_ok=True)
    log_path = log_dir / f"{identifier}.log"
    environment = dict(os.environ)
    environment.pop("MAKEFLAGS", None)
    environment.pop("MFLAGS", None)
    started = time.monotonic()
    with log_path.open("w", encoding="utf-8") as log:
        result = subprocess.run(
            list(arguments),
            env=environment,
            text=True,
            stdout=log,
            stderr=subprocess.STDOUT,
            check=False,
        )
    elapsed = time.monotonic() - started
    return {
        "id": identifier,
        "status": "passed" if result.returncode == 0 else "failed",
        "exit_code": result.returncode,
        "duration_seconds": round(elapsed, 3),
        "log": str(log_path),
        "command": list(arguments),
    }


def skipped_stage(identifier: str, reason: str) -> dict[str, Any]:
    return {
        "id": identifier,
        "status": "skipped",
        "reason": reason,
        "duration_seconds": 0.0,
    }


def stage_passed(stages: list[dict[str, Any]], identifier: str) -> bool:
    return next(stage for stage in stages if stage["id"] == identifier)["status"] == "passed"


def classify_stages(stages: list[dict[str, Any]], profile: str) -> dict[str, Any]:
    """Attribute a canary result without conflating upstream and integration."""
    failed = [stage for stage in stages if stage["status"] == "failed"]
    reference_ready = stage_passed(stages, "reference_build")
    product_ready = stage_passed(stages, "generated_product_build")
    smoke_ready = stage_passed(stages, "threaded_runtime_smoke")
    isolation_ready = stage_passed(stages, "thread_isolation_check")
    if not reference_ready:
        classification = "upstream_or_toolchain_failure"
    elif not product_ready or not smoke_ready:
        classification = "postgamma_integration_failure"
    elif failed:
        classification = "validation_failure"
    else:
        classification = "compatible"
    return {
        "classification": classification,
        "reference_build_ready": reference_ready,
        "scan_ready": stage_passed(stages, "candidate_facts"),
        "product_build_ready": product_ready,
        "runtime_smoke_ready": smoke_ready,
        "thread_isolation_ready": isolation_ready,
        "validation_ready": (
            isolation_ready
            if profile == "daily"
            else all(
                stage_passed(stages, identifier)
                for identifier in (
                    "reference_check_world",
                    "private_runtime_sanitizers",
                    "generated_check_world",
                    "thread_isolation_check",
                    "thread_isolation_stress",
                )
            )
        ),
        "first_failed_stage": failed[0]["id"] if failed else None,
    }


def project_fingerprint(root: Path) -> dict[str, Any]:
    status = command_output(
        ("git", "status", "--porcelain=v1", "--untracked-files=all"), root
    )
    return {
        "commit": command_output(("git", "rev-parse", "HEAD"), root),
        "dirty": bool(status),
    }


def write_report(path: Path, document: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        json.dumps(document, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )


def run_canary(args: argparse.Namespace) -> tuple[Path, dict[str, Any]]:
    root = args.project_root.resolve()
    repository = args.postgres_source.resolve()
    adapter_path = args.adapter.resolve()
    build_root = args.build_root.resolve()
    candidate_commit = git(repository, "rev-parse", "--verify", f"{args.ref}^{{commit}}")
    short_commit = candidate_commit[:12]
    candidate_root = build_root / "upstream-canary" / "candidates" / short_commit
    candidate_build = candidate_root / "build"
    worktree = build_root / "upstream-canary" / "worktrees" / f"postgres-{short_commit}"
    ensure_candidate_worktree(repository, candidate_commit, worktree)
    adapter = load_adapter(adapter_path)
    candidate_major, declared_product_major = inspect_source_compatibility(
        adapter, worktree
    )
    upstream = load_json(args.upstream_manifest.resolve())
    candidate_manifest_path = candidate_root / "input/upstream.json"
    write_json_if_changed(
        candidate_manifest_path,
        {
            "schema_version": 1,
            "repository": upstream["repository"],
            "path": "postgres",
            "ref_name": args.ref,
            "commit": candidate_commit,
        },
    )
    assignments: dict[str, Path | str] = {
        "BUILD_DIR": candidate_build,
        "PG_SOURCE": worktree,
        "UPSTREAM_MANIFEST": candidate_manifest_path,
        "POSTGRES_ADAPTER": adapter_path,
        "POSTGRES_CANDIDATE_PROBE": "1",
        "POSTGRES_INTEGRATION_PROBE": "0",
        "POSTGAMMA_SUPPORT_PROFILE": "product",
        "BACKEND_STATE_RULES": args.state_rules.resolve(),
        "PG_CONFIGURE_ARGS": args.configure_arguments,
    }
    stages: list[dict[str, Any]] = []
    log_dir = candidate_root / "logs" / args.profile

    reference = run_stage(
        "reference_build",
        make_arguments(args.make, root, args.jobs, assignments, ("reference-build",)),
        log_dir,
    )
    stages.append(reference)
    if reference["status"] == "passed":
        scan = run_stage(
            "candidate_scan",
            make_arguments(
                args.make,
                root,
                args.jobs,
                assignments,
                ("guc-catalog",),
            ),
            log_dir,
        )
        stages.append(scan)
    else:
        stages.append(skipped_stage("candidate_scan", "reference build failed"))

    scan_policy_path = candidate_root / "input/gucs-scan-policy.json"
    state_policy_path = candidate_root / "input/backend-state-probe.json"
    inheritance_policy_path = candidate_root / "input/backend-inheritance-probe.json"
    if stage_passed(stages, "candidate_scan"):
        write_json_if_changed(
            scan_policy_path,
            scan_policy(
                load_json(candidate_build / "catalog/gucs.json"),
                load_json(args.guc_policy.resolve()),
            ),
        )
        assignments["GUC_OWNERSHIP"] = scan_policy_path
        facts = run_stage(
            "candidate_facts",
            make_arguments(
                args.make,
                root,
                args.jobs,
                assignments,
                ("state-inventory", "ast-scan", "backend-inheritance-catalog"),
            ),
            log_dir,
        )
        stages.append(facts)
    else:
        stages.append(skipped_stage("candidate_facts", "candidate scan failed"))

    if stage_passed(stages, "candidate_facts"):
        candidate_state_policy = propose(
            load_json(candidate_build / "catalog/backend-state.json"),
            load_json(candidate_build / "inventory/gucs.json"),
            None,
            load_json(args.state_rules.resolve()),
            load_json(args.state_policy.resolve()),
        )
        write_json_if_changed(state_policy_path, candidate_state_policy)
        write_json_if_changed(
            inheritance_policy_path,
            candidate_inheritance_policy(
                load_json(candidate_build / "catalog/backend-inheritance.json"),
                load_json(args.inheritance_policy.resolve()),
            ),
        )
        assignments["BACKEND_STATE_OWNERSHIP"] = state_policy_path
        assignments["BACKEND_INHERITANCE_POLICY"] = inheritance_policy_path
        product = run_stage(
            "generated_product_build",
            make_arguments(
                args.make, root, args.jobs, assignments, ("generated-build",)
            ),
            log_dir,
        )
        stages.append(product)
    else:
        stages.append(skipped_stage("generated_product_build", "candidate facts failed"))

    if stage_passed(stages, "generated_product_build"):
        smoke = run_stage(
            "threaded_runtime_smoke",
            make_arguments(
                args.make,
                root,
                args.jobs,
                assignments,
                ("generated-thread-runtime-check",),
            ),
            log_dir,
        )
        stages.append(smoke)
        stages.append(
            run_stage(
                "thread_isolation_check",
                make_arguments(
                    args.make,
                    root,
                    args.jobs,
                    assignments,
                    ("thread-isolation-check",),
                ),
                log_dir,
            )
        )
    else:
        stages.append(
            skipped_stage("threaded_runtime_smoke", "generated product build failed")
        )
        stages.append(
            skipped_stage("thread_isolation_check", "generated product build failed")
        )

    if args.profile == "weekly" and stage_passed(stages, "reference_build"):
        stages.append(
            run_stage(
                "reference_check_world",
                make_arguments(
                    args.make,
                    root,
                    args.jobs,
                    assignments,
                    ("reference-check-world",),
                ),
                log_dir,
            )
        )
        stages.append(
            run_stage(
                "private_runtime_sanitizers",
                make_arguments(
                    args.make,
                    root,
                    args.jobs,
                    assignments,
                    ("runtime-sanitizer-test",),
                ),
                log_dir,
            )
        )
    else:
        for identifier in ("reference_check_world", "private_runtime_sanitizers"):
            stages.append(
                skipped_stage(
                    identifier, "daily profile or prerequisite failed"
                )
            )

    if args.profile == "weekly" and stage_passed(stages, "generated_product_build"):
        validation_assignments = dict(assignments)
        validation_assignments["POSTGAMMA_SUPPORT_PROFILE"] = "validation"
        stages.append(
            run_stage(
                "generated_check_world",
                make_arguments(
                    args.make,
                    root,
                    args.jobs,
                    validation_assignments,
                    ("generated-check-world",),
                ),
                log_dir,
            )
        )
        stages.append(
            run_stage(
                "thread_isolation_stress",
                make_arguments(
                    args.make,
                    root,
                    args.jobs,
                    {
                        **assignments,
                        "ISOLATION_STRESS_SECONDS": str(args.stress_seconds),
                    },
                    ("thread-isolation-stress",),
                ),
                log_dir,
            )
        )
    else:
        stages.append(
            skipped_stage(
                "generated_check_world", "daily profile or prerequisite failed"
            )
        )
        stages.append(
            skipped_stage(
                "thread_isolation_stress", "daily profile or prerequisite failed"
            )
        )

    summary = classify_stages(stages, args.profile)
    report = {
        "schema_version": 1,
        "kind": REPORT_KIND,
        "profile": args.profile,
        "project": project_fingerprint(root),
        "candidate": {
            "requested_ref": args.ref,
            "commit": candidate_commit,
            "postgresql_major": candidate_major,
            "declared_product_major": declared_product_major,
            "worktree": str(worktree),
        },
        "adapter": {"path": str(adapter_path), "sha256": sha256(adapter_path)},
        "engine": {"sha256": source_digest(root)},
        "build_configuration": {
            "configure_arguments": args.configure_arguments,
            "jobs": args.jobs,
            "toolchain": {
                "make": tool_identity((args.make, "--version"), root),
                "cc": tool_identity((os.environ.get("CC", "cc"), "--version"), root),
                "python": tool_identity((sys.executable, "--version"), root),
            },
        },
        "probe_policies": {
            "reviewed": False,
            "guc": str(scan_policy_path),
            "backend_state": str(state_policy_path),
            "backend_inheritance": str(inheritance_policy_path),
        },
        "stages": stages,
        "summary": summary,
    }
    output = (
        args.output.resolve()
        if args.output is not None
        else build_root / "reports" / f"upstream-canary-{short_commit}-{args.profile}.json"
    )
    write_report(output, report)
    return output, report


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--project-root", required=True, type=Path)
    parser.add_argument("--postgres-source", required=True, type=Path)
    parser.add_argument("--upstream-manifest", required=True, type=Path)
    parser.add_argument("--adapter", required=True, type=Path)
    parser.add_argument("--guc-policy", required=True, type=Path)
    parser.add_argument("--state-policy", required=True, type=Path)
    parser.add_argument("--state-rules", required=True, type=Path)
    parser.add_argument("--inheritance-policy", required=True, type=Path)
    parser.add_argument("--build-root", required=True, type=Path)
    parser.add_argument("--ref", required=True)
    parser.add_argument("--profile", choices=PROFILES, default="daily")
    parser.add_argument("--stress-seconds", type=float, default=3600.0)
    parser.add_argument("--configure-arguments", default="")
    parser.add_argument("--make", default="make")
    parser.add_argument("--jobs", type=int, default=2)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    if args.jobs <= 0:
        parser.error("--jobs must be positive")
    if not args.ref:
        parser.error("--ref is required; for example --ref origin/master")
    if args.stress_seconds <= 0:
        parser.error("--stress-seconds must be positive")
    try:
        output, report = run_canary(args)
    except (CanaryError, OSError, ValueError, subprocess.SubprocessError) as exc:
        parser.error(str(exc))
    summary = report["summary"]
    print(
        f"upstream canary: {summary['classification']}; "
        f"candidate={report['candidate']['commit'][:12]}; report={output}"
    )
    return 0 if summary["classification"] == "compatible" else 1


if __name__ == "__main__":
    raise SystemExit(main())
