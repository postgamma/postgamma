#!/usr/bin/env python3
"""Validate the tested-byte promotion and manylinux workflow contract."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
from pathlib import Path

from check_python_wheel import WheelCheckError, load_release_baseline


REPORT_KIND = "postgamma.python-release-workflow-contract"
EXPECTED_PYTHON_TAGS = ["cp310", "cp311", "cp312", "cp313", "cp314"]
EXPECTED_POLICY = "manylinux_2_28_x86_64"
EXPECTED_IMAGE = "quay.io/pypa/manylinux_2_28_x86_64:2026.05.07-2"
EXPECTED_GNU_MAKE = "4.4.1"
EXPECTED_GNU_MAKE_SHA256 = (
    "dd16fb1d67bfab79a72f5e8390735c49e3e8e70b4945a15ab1f81ddb78658fb3"
)


class WorkflowContractError(RuntimeError):
    """The release workflow no longer implements the reviewed contract."""


def validate_pypi_workflow(path: Path) -> dict[str, object]:
    content = path.read_text(encoding="utf-8")
    required = {
        "published release trigger": "types:\n      - published",
        "explicit owner switch": (
            "vars.POSTGAMMA_PYPI_PUBLISH_ENABLED == 'true'"
        ),
        "trusted environment": "name: pypi",
        "OIDC authority": "id-token: write",
        "release download": "gh release download",
        "canonical version": "buildsys/product_version.py",
        "exact wheel verification": (
            "buildsys/prepare_release_assets.py verify-wheels"
        ),
        "Trusted Publishing action": (
            "pypa/gh-action-pypi-publish@release/v1"
        ),
        "isolated package directory": "packages-dir: dist/",
    }
    missing = [name for name, marker in required.items() if marker not in content]
    if missing:
        raise WorkflowContractError(
            "PyPI workflow is missing: " + ", ".join(missing)
        )
    forbidden = ("TWINE_PASSWORD", "secrets.PYPI", "__token__")
    if any(marker in content for marker in forbidden):
        raise WorkflowContractError(
            "PyPI workflow must use OIDC instead of a stored upload token"
        )
    if content.count("id-token: write") != 1:
        raise WorkflowContractError("only the PyPI publication job may request OIDC")
    return {
        "workflow_sha256": hashlib.sha256(content.encode()).hexdigest(),
        "trigger": "release.published",
        "environment": "pypi",
        "trusted_publishing": True,
        "owner_switch_required": True,
        "exact_github_release_wheels": True,
        "stored_upload_token": False,
    }


def check(
    path: Path, baseline_path: Path, pypi_path: Path
) -> dict[str, object]:
    content = path.read_text(encoding="utf-8")
    baseline = load_release_baseline(baseline_path)
    pypi = validate_pypi_workflow(pypi_path)
    if (
        baseline["python"]["tags"] != EXPECTED_PYTHON_TAGS
        or baseline["python"]["manylinux_policy"] != EXPECTED_POLICY
    ):
        raise WorkflowContractError(
            "workflow constants differ from the Python release baseline"
        )
    required = {
        "pinned manylinux image": EXPECTED_IMAGE,
        "manylinux policy": f"MANYLINUX_POLICY: {EXPECTED_POLICY}",
        "pinned GNU Make": f'GNU_MAKE_VERSION: "{EXPECTED_GNU_MAKE}"',
        "pinned GNU Make checksum": (
            f"GNU_MAKE_SHA256: {EXPECTED_GNU_MAKE_SHA256}"
        ),
        "GNU Make source URL": (
            '"https://ftp.gnu.org/gnu/make/make-${GNU_MAKE_VERSION}.tar.gz"'
        ),
        "GNU Make checksum verification": "sha256sum --check",
        "GNU Make install prefix": "./configure --prefix=/opt/postgamma-tools",
        "GNU Make path": 'echo "/opt/postgamma-tools/bin" >> "$GITHUB_PATH"',
        "auditwheel repair": "buildsys/repair_python_wheel.py",
        "static kernel archive": "POSTGAMMA_STATIC_LIBRARY=",
        "static kernel link closure": "POSTGAMMA_STATIC_LINK_OPTIONS=",
        "static kernel receipt": "POSTGAMMA_STATIC_RECEIPT=",
        "clean pip install": "--installer pip",
        "platform execution gate": "--require-platform \"$MANYLINUX_POLICY\"",
        "matrix aggregation": "buildsys/check_python_release.py",
        "non-publishing evidence": "python-release-evidence.json",
        "canonical version": "buildsys/product_version.py",
        "canonical release tag": '- "v*"',
        "tested-byte promotion": "buildsys/prepare_release_assets.py prepare",
        "release bundle": "postgamma-release-${{ needs.version.outputs.version }}",
        "protected release environment": "environment: github-release",
        "draft release": "--verify-tag --draft --generate-notes",
        "GitHub release publication": "gh release edit",
        "strict release documentation": "make docs-check",
    }
    missing = [name for name, marker in required.items() if marker not in content]
    if missing:
        raise WorkflowContractError(
            "Python release workflow is missing: " + ", ".join(missing)
        )
    if "libpostgamma_python_abi1.so" in content or "POSTGAMMA_LIBRARY=" in content:
        raise WorkflowContractError(
            "Python release workflow must not package a PostGamma shared library"
        )
    baseline_marker = "--baseline manifests/api/python-api-v1.json"
    if content.count(baseline_marker) != 2:
        raise WorkflowContractError(
            "Python release workflow must bind wheel and aggregate evidence "
            "to the Python release baseline"
        )
    matrix_match = re.search(
        r"python_tag:\s*\n(?P<body>(?:\s+- cp[0-9]+\s*\n)+)", content
    )
    if matrix_match is None:
        raise WorkflowContractError("Python release workflow has no CPython matrix")
    tags = re.findall(r"- (cp[0-9]+)", matrix_match.group("body"))
    if tags != EXPECTED_PYTHON_TAGS:
        raise WorkflowContractError(
            f"Python release matrix is {tags}, expected {EXPECTED_PYTHON_TAGS}"
        )
    if re.search(r"(?im)\btwine\s+upload\b", content) or (
        "gh-action-pypi-publish" in content
    ):
        raise WorkflowContractError(
            "GitHub release workflow must not publish directly to a package index"
        )
    if content.count("contents: write") != 2:
        raise WorkflowContractError(
            "only the draft staging and protected publication jobs may write"
        )
    return {
        "schema_version": 1,
        "kind": REPORT_KIND,
        "status": "pass",
        "workflow_sha256": hashlib.sha256(content.encode()).hexdigest(),
        "manylinux_policy": EXPECTED_POLICY,
        "container_image": EXPECTED_IMAGE,
        "gnu_make_version": EXPECTED_GNU_MAKE,
        "gnu_make_source_sha256": EXPECTED_GNU_MAKE_SHA256,
        "python_tags": tags,
        "kernel_build_count": 1,
        "kernel_linkage": "static",
        "separate_kernel_library_in_wheel": False,
        "wheel_build_count": len(tags),
        "clean_install_required": True,
        "exact_tested_byte_promotion": True,
        "github_release_declared": True,
        "release_environment": "github-release",
        "draft_before_approval": True,
        "strict_documentation_required": True,
        "build_jobs_publish_authorized": False,
        "pypi": pypi,
        "publish_authorized": False,
        "execution_evidence_required": "postgamma.python-release-evidence",
        "release_baseline_sha256": hashlib.sha256(
            baseline_path.read_bytes()
        ).hexdigest(),
        "release": baseline["release"],
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--workflow", required=True, type=Path)
    parser.add_argument("--pypi-workflow", required=True, type=Path)
    parser.add_argument("--baseline", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    try:
        report = check(
            args.workflow.resolve(),
            args.baseline.resolve(),
            args.pypi_workflow.resolve(),
        )
    except (OSError, WheelCheckError, WorkflowContractError) as error:
        parser.error(str(error))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(
        json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    print(
        "Python release workflow contract: one tested manylinux kernel, "
        "five clean-installed wheels, and protected exact-byte promotion"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
