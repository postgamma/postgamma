#!/usr/bin/env python3
"""Skip scheduled CI tests only when the same source combination has passed."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import subprocess
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path
from typing import Any


def git(root: Path, *arguments: str) -> str:
    return subprocess.check_output(
        ["git", "-C", str(root), *arguments], text=True, timeout=30
    ).strip()


def source_inputs(root: Path, profile: str, branch: str) -> dict[str, str]:
    postgres_url = os.environ.get("PG_REPOSITORY") or git(
        root, "config", "-f", ".gitmodules", "submodule.postgres.url"
    )
    pgvector_url = os.environ.get("PGVECTOR_REPOSITORY") or git(
        root, "config", "-f", ".gitmodules", "submodule.third_party/pgvector.url"
    )
    commit = ""
    if branch:
        ref = f"refs/heads/{branch}"
        git(root, "check-ref-format", ref)
        rows = git(root, "ls-remote", "--exit-code", "--", postgres_url, ref).splitlines()
        if len(rows) != 1 or rows[0].split()[1:] != [ref]:
            raise ValueError(f"could not resolve exactly one PostgreSQL branch: {branch}")
        commit = rows[0].split()[0]
        if not re.fullmatch(r"[0-9a-f]{40}|[0-9a-f]{64}", commit):
            raise ValueError("PostgreSQL branch did not resolve to a commit ID")
    return {
        "postgamma_commit": git(root, "rev-parse", "HEAD"),
        "postgres_commit": commit,
        "postgres_url": postgres_url,
        "pgvector_url": pgvector_url,
        "branch": branch,
        "profile": profile,
    }


def test_identity(inputs: dict[str, str]) -> str:
    digest = hashlib.sha256(json.dumps(inputs, sort_keys=True).encode()).hexdigest()
    return f"{inputs['branch'] or 'embedded-product'} {inputs['profile']} {digest}"


class GitHub:
    def __init__(self, repository: str) -> None:
        self.base = os.environ.get("GITHUB_API_URL", "https://api.github.com").rstrip("/")
        self.repository = repository

    def get(self, path: str, **parameters: Any) -> dict[str, Any]:
        query = urllib.parse.urlencode(parameters)
        request = urllib.request.Request(
            f"{self.base}/repos/{self.repository}/{path}?{query}",
            headers={
                "Accept": "application/vnd.github+json",
                "X-GitHub-Api-Version": "2022-11-28",
            },
        )
        if token := os.environ.get("GH_TOKEN"):
            request.add_header("Authorization", f"Bearer {token}")
        with urllib.request.urlopen(request, timeout=30) as response:
            return json.load(response)


def previous_result(api: GitHub, commit: str, identity: str, run_id: str) -> str:
    runs = api.get(
        "actions/workflows/upstream-canary.yml/runs",
        head_sha=commit, status="completed", per_page=100,
    )["workflow_runs"]
    results = []
    for run in runs:
        if str(run["id"]) == run_id or run["head_sha"] != commit:
            continue
        # Include older attempts: a manual retry can supersede a previous result.
        page = 1
        while True:
            jobs = api.get(
                f"actions/runs/{run['id']}/jobs", filter="all", per_page=100, page=page
            )["jobs"]
            for job in jobs:
                if job.get("head_sha") != commit or job.get("status") != "completed":
                    continue
                for step in job.get("steps", []):
                    if step.get("name") != f"Test {identity}":
                        continue
                    if step.get("conclusion") == "skipped" and job["conclusion"] == "success":
                        continue
                    conclusion = (
                        "success" if step.get("conclusion") == "success"
                        and job["conclusion"] == "success" else "failure"
                    )
                    results.append((job["completed_at"], job["id"], conclusion))
            if len(jobs) < 100:
                break
            page += 1
    return max(results)[2] if results else "missing"


def should_run(api: GitHub, inputs: dict[str, str], event: str,
               run_id: str, attempt: int) -> tuple[bool, str]:
    if event != "schedule" or attempt > 1:
        return True, "manual run or retry"
    try:
        result = previous_result(api, inputs["postgamma_commit"], test_identity(inputs), run_id)
    except (OSError, ValueError, KeyError, TypeError) as error:
        return True, f"test history unavailable ({type(error).__name__})"
    if result == "success":
        return False, "unchanged source combination already passed"
    return True, f"previous result: {result}"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--profile", choices=("daily", "weekly"), required=True)
    parser.add_argument("--branch", default="", help="omit for the pinned embedded product")
    args = parser.parse_args()
    try:
        inputs = source_inputs(args.root, args.profile, args.branch)
        run, reason = should_run(
            GitHub(os.environ.get("GITHUB_REPOSITORY", "")), inputs,
            os.environ.get("GITHUB_EVENT_NAME", "workflow_dispatch"),
            os.environ.get("GITHUB_RUN_ID", ""),
            int(os.environ.get("GITHUB_RUN_ATTEMPT", "1")),
        )
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        parser.error(str(error))
    outputs = {
        "run": str(run).lower(),
        "identity": test_identity(inputs),
        "postgres_commit": inputs["postgres_commit"],
    }
    if output := os.environ.get("GITHUB_OUTPUT"):
        with Path(output).open("a", encoding="utf-8") as stream:
            for name, value in outputs.items():
                stream.write(f"{name}={value}\n")
    summary = (
        f"{args.branch or 'embedded-product'} ({args.profile}): "
        f"{'run' if run else 'skip'}; {reason}.\n"
    )
    print(summary, end="")
    if output := os.environ.get("GITHUB_STEP_SUMMARY"):
        with Path(output).open("a", encoding="utf-8") as stream:
            stream.write(summary)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
