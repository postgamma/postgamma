#!/usr/bin/env python3
"""Validate the source-only superproject layout."""

from __future__ import annotations

import argparse
from pathlib import Path


REQUIRED_PATHS = (
    ".gitmodules",
    "VERSION",
    "compiler",
    "manifests/upstream.json",
    "manifests/postgresql/adapter.json",
    "manifests/postgresql/embedded.json",
    "manifests/profiles/embedded-bootstrap.json",
    "manifests/extensions/bootstrap-static-extension.json",
    "manifests/extensions/pgvector-upstream.json",
    "manifests/domains/pgvector.json",
    "manifests/ownership/extensions/pgvector.json",
    "manifests/ownership/backend-state-rules.json",
    "manifests/ownership/backend-state-mobility.json",
    "manifests/ownership/frontend-tool-state.json",
    "mkdocs.yml",
    "docs/requirements.txt",
    "docs/reference/c-api.json",
    "docs/reference/python-api.json",
    "buildsys/check_documentation.py",
    "buildsys/product_version.py",
    "buildsys/prepare_release_assets.py",
    "buildsys/generate_c_api_docs.py",
    "buildsys/generate_python_api_docs.py",
    "buildsys/mkdocs_hooks.py",
    ".github/workflows/docs.yml",
    ".github/workflows/python-wheels.yml",
    ".github/workflows/pypi.yml",
    "docs/downloads.md",
    "docs/releases/versioning.md",
    "embedded-c/Makefile",
    "embedded-c/include/postgamma/private/bootstrap_probe.h",
    "embedded-c/include/postgamma/private/static_module_registry.h",
    "embedded-c/src/static_module_registry.c",
    "embedded-c/src/bootstrap_probe.c",
    "embedded-c/libpostgamma-bootstrap.map",
    "embedded-c/tests/test_bootstrap_probe_header.c",
    "tests/embedded/bootstrap/bootstrap_extension.c",
    "runtime/src",
    "extensions/pgvector/src/pgvector_adapter.c",
    "examples/python",
    "tests/buildsys/test_documentation.py",
    "tests/buildsys/test_product_version.py",
    "tests/buildsys/test_prepare_release_assets.py",
    "tests/buildsys/test_release_workflows.py",
    "tests",
)

SUBMODULE_PATHS = (
    "postgres/.git",
    "third_party/pgvector/.git",
)

POSTGAMMA_SOURCE_ROOTS = (
    "buildsys",
    "compiler",
    "embedded-c",
    "runtime",
    "tests",
)
FOREIGN_SCRIPT_SUFFIXES = {
    ".awk",
    ".bash",
    ".fish",
    ".lua",
    ".php",
    ".pl",
    ".pm",
    ".ps1",
    ".r",
    ".rb",
    ".sed",
    ".sh",
    ".tcl",
    ".zsh",
}


def is_foreign_script(path: Path) -> bool:
    if path.suffix.lower() in FOREIGN_SCRIPT_SUFFIXES:
        return True
    try:
        with path.open("rb") as source:
            first_line = source.readline(256).lower()
    except OSError:
        return False
    return first_line.startswith(b"#!") and b"python" not in first_line


def foreign_scripts(root: Path) -> list[Path]:
    scripts: list[Path] = []
    for name in POSTGAMMA_SOURCE_ROOTS:
        source_root = root / name
        if not source_root.is_dir():
            continue
        scripts.extend(
            path.relative_to(root)
            for path in source_root.rglob("*")
            if path.is_file() and is_foreign_script(path)
        )
    return sorted(scripts)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", required=True, type=Path)
    args = parser.parse_args()

    root = args.root.resolve()
    missing_submodules = [
        name for name in SUBMODULE_PATHS if not (root / name).exists()
    ]
    if missing_submodules:
        parser.error(
            "source submodules are not initialized: "
            + ", ".join(missing_submodules)
            + "; run `git submodule update --init --recursive` from the project root"
        )
    missing = [name for name in REQUIRED_PATHS if not (root / name).exists()]
    if missing:
        parser.error("missing required path(s): " + ", ".join(missing))

    if (root / "postgres" / ".gitmodules").exists():
        parser.error("postgres/ looks like a superproject, not the expected submodule")

    scripts = foreign_scripts(root)
    if scripts:
        parser.error(
            "postgamma-owned scripts must be Python: "
            + ", ".join(str(path) for path in scripts)
        )

    print(f"layout: ok ({root})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
