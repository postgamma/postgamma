#!/usr/bin/env python3
"""Initialize pinned source submodules with checkout-local download URLs."""

from __future__ import annotations

import argparse
import os
import subprocess
from pathlib import Path


def git(repository: Path, *arguments: str) -> None:
    subprocess.run(["git", "-C", str(repository), *arguments], check=True)


def prepare_sources(root: Path, postgres_url: str, pgvector_url: str) -> None:
    modules = (("postgres", postgres_url), ("third_party/pgvector", pgvector_url))
    for name, url in modules:
        if not url:
            continue
        git(root, "config", "--local", f"submodule.{name}.url", url)
        repository = root / name
        if (repository / ".git").exists():
            git(repository, "remote", "set-url", "origin", url)
        else:
            stored = subprocess.check_output(
                ["git", "-C", str(root), "rev-parse", "--git-path", f"modules/{name}"],
                text=True,
            ).strip()
            git_dir = root / stored
            if git_dir.is_dir():
                subprocess.run(
                    ["git", "--git-dir", str(git_dir), "remote", "set-url", "origin", url],
                    check=True,
                )
    git(
        root, "submodule", "update", "--init", "--recursive", "--checkout",
        "--", *(name for name, _url in modules),
    )


def fetch_branch(repository: Path, branch: str) -> None:
    ref = f"refs/heads/{branch}"
    git(repository, "check-ref-format", ref)
    git(
        repository, "fetch", "--no-tags", "origin",
        f"+{ref}:refs/remotes/origin/{branch}",
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", required=True, type=Path)
    parser.add_argument("--postgres-url", default=os.environ.get("PG_REPOSITORY", ""))
    parser.add_argument("--pgvector-url", default=os.environ.get("PGVECTOR_REPOSITORY", ""))
    parser.add_argument("--postgres-source", type=Path)
    parser.add_argument("--fetch-branch")
    args = parser.parse_args()
    root = args.root.resolve()
    try:
        prepare_sources(root, args.postgres_url, args.pgvector_url)
        if args.fetch_branch:
            repository = args.postgres_source.resolve() if args.postgres_source else root / "postgres"
            fetch_branch(repository, args.fetch_branch)
    except (OSError, subprocess.CalledProcessError) as error:
        parser.error(str(error))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
