#!/usr/bin/env python3
"""Initialize pinned source submodules with checkout-local download URLs."""

from __future__ import annotations

import argparse
import json
import os
import subprocess
from pathlib import Path


def git(repository: Path, *arguments: str) -> None:
    subprocess.run(
        [
            "git", "-c", "protocol.file.allow=always", "-C", str(repository),
            *arguments,
        ],
        check=True,
    )


def configured_url(root: Path, name: str) -> str:
    key = f"submodule.{name}.url"
    configured = subprocess.run(
        ["git", "-C", str(root), "config", "--local", "--get", key],
        text=True,
        stdout=subprocess.PIPE,
        check=False,
    )
    if configured.returncode == 0:
        return configured.stdout.strip()
    return subprocess.check_output(
        ["git", "-C", str(root), "config", "-f", ".gitmodules", "--get", key],
        text=True,
    ).strip()


def set_module_url(root: Path, name: str, url: str) -> None:
    if url:
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


def initialize_module(root: Path, name: str, urls: tuple[str, ...]) -> None:
    candidates = tuple(dict.fromkeys(url for url in urls if url))
    last_error: subprocess.CalledProcessError | None = None
    for url in candidates:
        set_module_url(root, name, url)
        try:
            git(
                root, "submodule", "update", "--init", "--recursive",
                "--checkout", "--", name,
            )
            return
        except subprocess.CalledProcessError as error:
            last_error = error
    if last_error is not None:
        raise last_error
    raise RuntimeError(f"no repository URL is configured for submodule {name}")


def prepare_sources(
    root: Path,
    postgres_url: str,
    pgvector_url: str,
    postgres_fallback_url: str = "",
    pgvector_fallback_url: str = "",
) -> None:
    modules = (
        ("postgres", postgres_url, postgres_fallback_url),
        ("third_party/pgvector", pgvector_url, pgvector_fallback_url),
    )
    for name, primary, fallback in modules:
        default = configured_url(root, name)
        initialize_module(root, name, (primary or default, fallback))
    verify_pgvector_identity(root, pgvector_fallback_url)


def git_output(repository: Path, *arguments: str) -> str:
    return subprocess.check_output(
        ["git", "-C", str(repository), *arguments], text=True,
    ).strip()


def verify_pgvector_identity(root: Path, fallback_url: str = "") -> None:
    manifest_path = root / "manifests/extensions/pgvector-upstream.json"
    if not manifest_path.is_file():
        return
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    repository = root / manifest["path"]
    commit = git_output(repository, "rev-parse", "HEAD")
    if commit != manifest["commit"]:
        raise RuntimeError(
            f"pgvector mirror checked out {commit}, expected {manifest['commit']}"
        )
    try:
        tag = git_output(repository, "describe", "--tags", "--exact-match", "HEAD")
    except subprocess.CalledProcessError:
        git(repository, "fetch", "--force", "--tags", "origin")
        try:
            tag = git_output(
                repository, "describe", "--tags", "--exact-match", "HEAD"
            )
        except subprocess.CalledProcessError:
            if not fallback_url:
                raise RuntimeError(
                    f"pgvector mirror omits required tag {manifest['ref_name']}"
                ) from None
            set_module_url(root, manifest["path"], fallback_url)
            git(repository, "fetch", "--force", "--tags", "origin")
            try:
                tag = git_output(
                    repository, "describe", "--tags", "--exact-match", "HEAD"
                )
            except subprocess.CalledProcessError as fallback_error:
                raise RuntimeError(
                    "primary and fallback pgvector mirrors omit required tag "
                    f"{manifest['ref_name']}"
                ) from fallback_error
    if tag != manifest["ref_name"]:
        raise RuntimeError(
            f"pgvector mirror resolved tag {tag}, expected {manifest['ref_name']}"
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
    parser.add_argument(
        "--postgres-fallback-url",
        default=os.environ.get("PG_REPOSITORY_FALLBACK", ""),
    )
    parser.add_argument("--pgvector-url", default=os.environ.get("PGVECTOR_REPOSITORY", ""))
    parser.add_argument(
        "--pgvector-fallback-url",
        default=os.environ.get("PGVECTOR_REPOSITORY_FALLBACK", ""),
    )
    parser.add_argument("--postgres-source", type=Path)
    parser.add_argument("--fetch-branch")
    args = parser.parse_args()
    root = args.root.resolve()
    try:
        prepare_sources(
            root,
            args.postgres_url,
            args.pgvector_url,
            args.postgres_fallback_url,
            args.pgvector_fallback_url,
        )
        if args.fetch_branch:
            repository = args.postgres_source.resolve() if args.postgres_source else root / "postgres"
            fetch_branch(repository, args.fetch_branch)
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        parser.error(str(error))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
