#!/usr/bin/env python3
"""Validate PostGamma's public identity, attribution, and terminology."""

from __future__ import annotations

import argparse
import ast
import hashlib
import json
import os
import re
import subprocess
from pathlib import Path
from typing import Iterable

from product_version import read_version


EVIDENCE_KIND = "postgamma.public-surface-evidence"
DISPLAY_BRAND = "PostGamma"
WORDMARK = "postgamma"
OFFICIAL_SITE = "https://postgamma.com"
COPYRIGHT_OWNER = "Shujie Zhang"
SOURCE_HEADER = (
    "/*\n"
    " * Copyright 2026 Shujie Zhang\n"
    " * SPDX-License-Identifier: Apache-2.0\n"
    " */\n\n"
)
APACHE_LICENSE_SHA256 = (
    "cfc7749b96f63bd31c3c42b5c471bf756814053e847c10f3eb003417bc523d30"
)
PRODUCT_SOURCE_DIRECTORIES = (
    "compiler/src",
    "embedded-c/include",
    "embedded-c/src",
    "extensions/pgvector/include",
    "extensions/pgvector/src",
    "python/src/postgamma",
    "runtime/include",
    "runtime/src",
    "examples/embedded_c",
)
PRODUCT_SOURCE_SUFFIXES = {".c", ".h", ".cpp"}
INCORRECT_BRAND = re.compile(r"\bPostgamma\b")
LOWERCASE_PROSE_BRAND = re.compile(
    r"(?<![./_-])\bpostgamma\b(?![./_-])"
)
INLINE_CODE = re.compile(r"(`+).*?\1")
WORDMARK_ELEMENT = re.compile(
    r'<span\b[^>]*\bclass="pg-brand-wordmark"[^>]*>' + WORDMARK + r"</span>"
)
HTML_TAG = re.compile(r"<[^>]*>")
TEXT_SUFFIXES = {".c", ".h", ".json", ".md", ".py", ".toml", ".yml"}


class PublicSurfaceError(RuntimeError):
    """The user-facing product identity or attribution contract changed."""


def public_files(root: Path) -> list[Path]:
    files = [
        root / "README.md",
        root / "CONTRIBUTING.md",
        root / "SECURITY.md",
        root / "NOTICE",
        root / "THIRD_PARTY_NOTICES",
        root / "mkdocs.yml",
        root / "python/README.md",
        root / "python/pyproject.toml",
        root / "manifests/api/python-api-v1.json",
        root / "embedded-c/src/postgamma.c",
    ]
    for directory in (
        root / "docs",
        root / "examples",
        root / "python/src/postgamma",
        root / "embedded-c/include/postgamma",
    ):
        for path in directory.rglob("*"):
            if (
                path.is_file()
                and path.suffix in TEXT_SUFFIXES
                and "private" not in path.relative_to(directory).parts
                and "__pycache__" not in path.parts
            ):
                files.append(path)
    return sorted(set(files))


def product_sources(root: Path) -> list[Path]:
    files: list[Path] = []
    for relative in PRODUCT_SOURCE_DIRECTORIES:
        directory = root / relative
        files.extend(
            path
            for path in directory.rglob("*")
            if path.is_file() and path.suffix in PRODUCT_SOURCE_SUFFIXES
        )
    return sorted(files)


def licensing_failures(root: Path) -> tuple[list[str], int]:
    failures: list[str] = []
    license_digest = hashlib.sha256((root / "LICENSE").read_bytes()).hexdigest()
    if license_digest != APACHE_LICENSE_SHA256:
        failures.append("LICENSE: not the canonical Apache-2.0 text")
    notice = (root / "NOTICE").read_text(encoding="utf-8")
    expected_notice = f"{DISPLAY_BRAND}\nCopyright 2026 {COPYRIGHT_OWNER}\n"
    if notice != expected_notice:
        failures.append("NOTICE: copyright owner or format changed")
    required_site_text = {
        "README.md": OFFICIAL_SITE,
        "python/README.md": OFFICIAL_SITE,
        "mkdocs.yml": f"site_url: {OFFICIAL_SITE}/",
        "python/build_backend.py": f'PROJECT_URL = "{OFFICIAL_SITE}"',
        "buildsys/build_static_library.py": f'"URL: {OFFICIAL_SITE}\\n"',
    }
    required_brand_text = {
        "README.md": f"# {DISPLAY_BRAND}\n",
        "python/README.md": f"# {DISPLAY_BRAND}\n",
        "mkdocs.yml": f"site_name: {WORDMARK}\n",
        "docs/Doxyfile": f'PROJECT_NAME           = "{DISPLAY_BRAND} C SDK"\n',
    }
    for relative, marker in required_site_text.items():
        if marker not in (root / relative).read_text(encoding="utf-8"):
            failures.append(f"{relative}: official project URL is missing")
    for relative, marker in required_brand_text.items():
        if marker not in (root / relative).read_text(encoding="utf-8"):
            failures.append(f"{relative}: expected brand marker {marker.strip()!r}")
    if (root / "docs/CNAME").read_text(encoding="utf-8") != "postgamma.com\n":
        failures.append("docs/CNAME: official domain changed")
    sources = product_sources(root)
    for path in sources:
        if not path.read_text(encoding="utf-8").startswith(SOURCE_HEADER):
            failures.append(
                f"{path.relative_to(root)}: Apache-2.0 copyright header is missing"
            )
    return failures, len(sources)


def incorrect_brand_lines(text: str) -> list[int]:
    return sorted(
        {
            text.count("\n", 0, match.start()) + 1
            for match in INCORRECT_BRAND.finditer(text)
        }
    )


def lowercase_prose_brand_lines(text: str) -> list[int]:
    """Find lowercase prose names outside code, wordmarks, and HTML attributes."""

    lines: list[int] = []
    fenced = False
    html_pre = False
    for number, line in enumerate(text.splitlines(), 1):
        stripped = line.lstrip()
        if stripped == f"site_name: {WORDMARK}":
            continue
        if stripped.startswith(("```", "~~~")):
            fenced = not fenced
            continue
        if "<pre" in line:
            html_pre = True
        if fenced or html_pre:
            if "</pre>" in line:
                html_pre = False
            continue
        prose = HTML_TAG.sub("", WORDMARK_ELEMENT.sub("", INLINE_CODE.sub("", line)))
        if LOWERCASE_PROSE_BRAND.search(prose):
            lines.append(number)
    return lines


def rendered_literals(node: ast.AST) -> Iterable[str]:
    """Yield only literal text rendered by an expression, not lookup keys."""
    if isinstance(node, ast.Constant) and isinstance(node.value, str):
        yield node.value
    elif isinstance(node, ast.JoinedStr):
        for value in node.values:
            if isinstance(value, ast.Constant) and isinstance(value.value, str):
                yield value.value
    elif isinstance(node, ast.BinOp) and isinstance(node.op, ast.Add):
        yield from rendered_literals(node.left)
        yield from rendered_literals(node.right)


def printed_brand_failures(root: Path) -> list[str]:
    failures: list[str] = []
    for path in sorted((root / "buildsys").glob("*.py")):
        tree = ast.parse(path.read_text(encoding="utf-8"), filename=str(path))
        for node in ast.walk(tree):
            if not (
                isinstance(node, ast.Call)
                and isinstance(node.func, ast.Name)
                and node.func.id == "print"
            ):
                continue
            if any(
                INCORRECT_BRAND.search(value)
                for argument in node.args
                for value in rendered_literals(argument)
            ):
                failures.append(
                    f"{path.relative_to(root)}:{node.lineno}: "
                    "incorrect Postgamma spelling in command output"
                )
    return failures


def public_help(root: Path, make: str) -> str:
    completed = subprocess.run(
        [make, "--no-print-directory", "help"],
        cwd=root,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    if completed.returncode != 0:
        raise PublicSurfaceError(
            f"cannot render public help ({completed.returncode}): "
            f"{completed.stderr.strip()}"
        )
    return completed.stdout


def build_identity(root: Path) -> str:
    source = (root / "embedded-c/src/postgamma.c").read_text(encoding="utf-8")
    required_expression = (
        'return "postgamma-" POSTGAMMA_PRODUCT_VERSION "-pg"\n'
        "\t\tPOSTGAMMA_POSTGRESQL_MAJOR;"
    )
    if required_expression not in source:
        raise PublicSurfaceError("cannot find the composed public C build identity")
    baseline = json.loads(
        (root / "manifests/api/python-api-v1.json").read_text(encoding="utf-8")
    )
    postgresql_major = baseline.get("postgresql", {}).get("major")
    identity = f"postgamma-{read_version(root).text}-pg{postgresql_major}"
    if baseline.get("postgresql", {}).get("build_id") != identity:
        raise PublicSurfaceError("C and Python build identities differ")
    return identity


def check(root: Path, make: str) -> dict[str, object]:
    failures: list[str] = []
    files = public_files(root)
    for path in files:
        if not path.is_file():
            failures.append(f"{path.relative_to(root)}: missing public file")
            continue
        try:
            text = path.read_text(encoding="utf-8")
        except UnicodeDecodeError as error:
            raise PublicSurfaceError(f"cannot decode public file {path}: {error}") from error
        for line in incorrect_brand_lines(text):
            failures.append(
                f"{path.relative_to(root)}:{line}: brand must be spelled PostGamma"
            )
        if path.suffix == ".md" or path.name in {
            "NOTICE",
            "THIRD_PARTY_NOTICES",
            "mkdocs.yml",
        }:
            for line in lowercase_prose_brand_lines(text):
                failures.append(
                    f"{path.relative_to(root)}:{line}: "
                    "display brand must be spelled PostGamma"
                )
    help_text = public_help(root, make)
    for line in incorrect_brand_lines(help_text):
        failures.append(f"make help:{line}: brand must be spelled PostGamma")
    failures.extend(printed_brand_failures(root))
    identity = build_identity(root)
    license_failures, copyrighted_source_count = licensing_failures(root)
    failures.extend(license_failures)
    if failures:
        raise PublicSurfaceError(
            "public surface contract violations:\n  " + "\n  ".join(failures)
        )
    return {
        "schema_version": 1,
        "kind": EVIDENCE_KIND,
        "status": "pass",
        "checked_file_count": len(files),
        "public_help_checked": True,
        "build_id": identity,
        "copyright_owner": COPYRIGHT_OWNER,
        "copyrighted_product_source_count": copyrighted_source_count,
        "official_site": OFFICIAL_SITE,
        "display_brand": DISPLAY_BRAND,
        "incorrect_brand_violations": 0,
        "lowercase_prose_brand_violations": 0,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", required=True, type=Path)
    parser.add_argument("--make", default=os.environ.get("MAKE", "make"))
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    try:
        report = check(args.root.resolve(strict=True), args.make)
        if args.output is not None:
            output = args.output.resolve()
            output.parent.mkdir(parents=True, exist_ok=True)
            output.write_text(
                json.dumps(report, indent=2, sort_keys=True) + "\n",
                encoding="utf-8",
            )
    except (OSError, ValueError, PublicSurfaceError) as error:
        parser.error(str(error))
    print("publication: pass")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
