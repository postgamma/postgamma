"""Build, preserve, and validate schema-2 Python wheel receipts.

This module checks receipt structure and identity against a release baseline.
Verification of actual wheel contents remains with the artifact checkers.
"""

from __future__ import annotations

import copy
import hashlib
import re
from pathlib import Path
from typing import Any


_FIXED_FIELDS = {
    "schema_version": 2,
    "kind": "postgamma.python-wheel-build",
    "kernel_linkage": "static",
    "bundled_kernel_library": False,
}
_TEXT_FIELDS = (
    "name",
    "version",
    "filename",
    "sha256",
    "platform_policy",
    "kernel_archive_sha256",
    "python",
    "metadata_version",
    "requires_python",
    "project_url",
)
_LIST_FIELDS = ("tags", "license_files")


class WheelReceiptError(RuntimeError):
    """A wheel receipt is missing required data or has malformed fields."""


def validate_receipt(document: object) -> None:
    """Require the common receipt fields without trusting them as evidence."""

    if not isinstance(document, dict):
        raise WheelReceiptError("wheel receipt must be a JSON object")
    required = set(_FIXED_FIELDS) | set(_TEXT_FIELDS) | set(_LIST_FIELDS) | {"size"}
    missing = required - document.keys()
    if missing:
        raise WheelReceiptError(
            "wheel receipt is missing required field(s): " + ", ".join(sorted(missing))
        )
    for name, expected in _FIXED_FIELDS.items():
        if type(document[name]) is not type(expected) or document[name] != expected:
            raise WheelReceiptError(f"wheel receipt {name} must be {expected!r}")
    for name in _TEXT_FIELDS:
        if not isinstance(document[name], str) or not document[name].strip():
            raise WheelReceiptError(f"wheel receipt {name} must be a nonempty string")
    for name in ("sha256", "kernel_archive_sha256"):
        if re.fullmatch(r"[0-9a-f]{64}", document[name]) is None:
            raise WheelReceiptError(f"wheel receipt {name} must be a SHA-256 hex digest")
    if type(document["size"]) is not int or document["size"] <= 0:
        raise WheelReceiptError("wheel receipt size must be a positive integer")
    for name in _LIST_FIELDS:
        values = document[name]
        if (
            not isinstance(values, list)
            or not values
            or any(not isinstance(value, str) or not value.strip() for value in values)
            or len(values) != len(set(values))
        ):
            raise WheelReceiptError(
                f"wheel receipt {name} must be a nonempty list of unique strings"
            )


def release_license_files(baseline: dict[str, Any]) -> dict[str, str]:
    """Return the complete project and third-party wheel license set."""

    wheel = baseline["wheel"]
    return {**wheel["project_license"], **wheel["third_party_licenses"]}


def validate_release_receipt(
    document: dict[str, Any], baseline: dict[str, Any]
) -> None:
    """Compare receipt identity with an already validated release baseline."""

    validate_receipt(document)
    expected = {
        "name": baseline["release"]["name"],
        "version": baseline["release"]["version"],
        "metadata_version": baseline["wheel"]["metadata_version"],
        "requires_python": baseline["python"]["requires_python"],
        "project_url": baseline["wheel"]["project_urls"]["Homepage"],
        "license_files": sorted(release_license_files(baseline)),
    }
    for name, value in expected.items():
        if document[name] != value:
            raise WheelReceiptError(
                f"wheel receipt {name} differs from the release baseline"
            )


def _artifact_fields(wheel: Path) -> dict[str, Any]:
    content = wheel.read_bytes()
    return {
        "filename": wheel.name,
        "sha256": hashlib.sha256(content).hexdigest(),
        "size": len(content),
    }


def build_receipt(
    wheel: Path,
    *,
    name: str,
    version: str,
    tags: list[str],
    platform_policy: str,
    kernel_archive_sha256: str,
    python: str,
    metadata_version: str,
    requires_python: str,
    project_url: str,
    license_files: list[str],
) -> dict[str, Any]:
    """Bind explicit build identity to the wheel bytes and canonical licenses."""

    document = {
        **_FIXED_FIELDS,
        **_artifact_fields(wheel),
        "name": name,
        "version": version,
        "tags": tags,
        "platform_policy": platform_policy,
        "kernel_archive_sha256": kernel_archive_sha256,
        "python": python,
        "metadata_version": metadata_version,
        "requires_python": requires_python,
        "project_url": project_url,
        "license_files": license_files,
    }
    validate_receipt(document)
    document["tags"] = list(tags)
    document["license_files"] = sorted(license_files)
    return document


def receipt_for_repaired_wheel(
    source: dict[str, Any],
    wheel: Path,
    *,
    tags: list[str],
    platform_policy: str,
) -> dict[str, Any]:
    """Preserve source identity, replacing only fields changed by repair."""

    validate_receipt(source)
    document = copy.deepcopy(source)
    document.update(_artifact_fields(wheel))
    document["tags"] = tags
    document["platform_policy"] = platform_policy
    validate_receipt(document)
    document["tags"] = list(tags)
    return document
