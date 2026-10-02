#!/usr/bin/env python3
"""Seal locally built static, Python, and documentation release artifacts."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
from typing import Any

from check_embedded_lifecycle import write_json


EVIDENCE_KIND = "postgamma.local-release-candidate"


class ReleaseCandidateError(RuntimeError):
    """The local release artifact set is incomplete or internally inconsistent."""


def load_document(path: Path, kind: str) -> dict[str, Any]:
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise ReleaseCandidateError(f"cannot read {path}: {error}") from error
    if (
        not isinstance(document, dict)
        or document.get("kind") != kind
        or document.get("status") != "pass"
    ):
        raise ReleaseCandidateError(f"{path} is not passing {kind} evidence")
    return document


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def site_identity(site: Path) -> dict[str, Any]:
    if not site.is_dir() or not (site / "index.html").is_file():
        raise ReleaseCandidateError("documentation site has no index.html")
    files = sorted(path for path in site.rglob("*") if path.is_file())
    if not files:
        raise ReleaseCandidateError("documentation site is empty")
    digest = hashlib.sha256()
    for path in files:
        relative = path.relative_to(site).as_posix().encode("utf-8")
        content_digest = bytes.fromhex(sha256(path))
        digest.update(len(relative).to_bytes(8, "big"))
        digest.update(relative)
        digest.update(content_digest)
    return {"file_count": len(files), "tree_sha256": digest.hexdigest()}


def close_candidate(
    *,
    root: Path,
    version: str,
    python_release_path: Path,
    conformance_path: Path,
    static_evidence_path: Path,
    static_receipt_path: Path,
    static_archive_path: Path,
    static_checksum_path: Path,
    wheel_receipt_path: Path,
    wheel_evidence_path: Path,
    wheel_directory: Path,
    docs_evidence_path: Path,
    publication_evidence_path: Path,
    artifact_publication_evidence_path: Path,
    docs_site: Path,
) -> dict[str, Any]:
    python_release = load_document(
        python_release_path, "postgamma.python-release-candidate"
    )
    conformance = load_document(
        conformance_path, "postgamma.embedded-conformance"
    )
    static_evidence = load_document(
        static_evidence_path, "postgamma.static-sdk-evidence"
    )
    static = load_document(static_receipt_path, "postgamma.static-sdk-archive")
    wheel = load_document(
        wheel_evidence_path, "postgamma.python-wheel-evidence"
    )
    docs = load_document(docs_evidence_path, "postgamma.documentation-evidence")
    load_document(
        publication_evidence_path, "postgamma.public-surface-evidence"
    )
    artifact_publication = load_document(
        artifact_publication_evidence_path,
        "postgamma.public-artifact-evidence",
    )
    try:
        wheel_receipt = json.loads(wheel_receipt_path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise ReleaseCandidateError(
            f"cannot read wheel receipt {wheel_receipt_path}: {error}"
        ) from error
    if (
        not isinstance(wheel_receipt, dict)
        or wheel_receipt.get("schema_version") != 2
        or wheel_receipt.get("kind") != "postgamma.python-wheel-build"
    ):
        raise ReleaseCandidateError("Python wheel build receipt is invalid")
    if (
        python_release.get("release", {}).get("version") != version
        or static.get("product_version") != version
        or wheel_receipt.get("version") != version
    ):
        raise ReleaseCandidateError("release artifact versions do not match")
    if (
        conformance.get("case_count") != 54
        or conformance.get("socket_libpq_matches_embedded_public_api") is not True
        or python_release.get("inputs", {}).get("embedded_release") is None
    ):
        raise ReleaseCandidateError("embedded product evidence is incomplete")

    static_archive = static_archive_path.resolve(strict=True)
    static_checksum = static_checksum_path.resolve(strict=True)
    static_digest = sha256(static_archive)
    expected_checksum = f"{static_digest}  {static_archive.name}\n"
    if (
        static.get("archive") != static_archive.name
        or static.get("archive_sha256") != static_digest
        or static_checksum.read_text(encoding="ascii") != expected_checksum
    ):
        raise ReleaseCandidateError("static SDK archive or checksum changed")
    tested_library_digest = static_evidence.get("archive", {}).get("sha256")
    if (
        not isinstance(tested_library_digest, str)
        or static.get("static_library_sha256") != tested_library_digest
    ):
        raise ReleaseCandidateError(
            "static SDK archive does not contain the tested library"
        )

    wheel_name = wheel_receipt.get("filename")
    if not isinstance(wheel_name, str) or Path(wheel_name).name != wheel_name:
        raise ReleaseCandidateError("wheel receipt filename is invalid")
    wheel_path = (wheel_directory / wheel_name).resolve(strict=True)
    wheel_digest = sha256(wheel_path)
    if (
        wheel_receipt.get("sha256") != wheel_digest
        or wheel.get("wheel", {}).get("sha256") != wheel_digest
        or wheel.get("wheel", {}).get("filename") != wheel_name
        or python_release.get("local_wheel", {}).get("sha256") != wheel_digest
    ):
        raise ReleaseCandidateError("Python wheel identity changed after testing")
    if wheel.get("gates", {}).get("integration", {}).get("scenario_count") != 16:
        raise ReleaseCandidateError("Python integration scenario set is incomplete")
    if docs.get("markdown_file_count", 0) < 30:
        raise ReleaseCandidateError("documentation evidence is incomplete")
    if (
        artifact_publication.get("build_path_violations") != 0
        or artifact_publication.get("static_sdk", {}).get("sha256") != static_digest
        or artifact_publication.get("python_wheel", {}).get("sha256") != wheel_digest
    ):
        raise ReleaseCandidateError("public artifact inspection is incomplete")
    documentation = site_identity(docs_site.resolve(strict=True))

    return {
        "schema_version": 1,
        "kind": EVIDENCE_KIND,
        "status": "pass",
        "release": {
            "name": "postgamma",
            "version": version,
            "state": "local-candidate",
            "publish_authorized": False,
            "public_release_ready": False,
        },
        "artifacts": {
            "static_sdk": {
                "path": str(static_archive.relative_to(root)),
                "sha256": static_digest,
                "tested": True,
            },
            "python_wheel": {
                "path": str(wheel_path.relative_to(root)),
                "sha256": wheel_digest,
                "tested": True,
                "integration_scenarios": 16,
            },
            "documentation": {
                "path": str(docs_site.resolve().relative_to(root)),
                **documentation,
                "tested": True,
            },
        },
        "embedded_conformance": {
            "cases": 54,
            "reference": "PostgreSQL 19 socket libpq",
            "candidate": "PostGamma embedded public C API",
            "semantic_match": True,
        },
        "evidence": {
            "python_release": sha256(python_release_path),
            "embedded_conformance": sha256(conformance_path),
            "static_sdk_validation": sha256(static_evidence_path),
            "static_sdk_receipt": sha256(static_receipt_path),
            "python_wheel_receipt": sha256(wheel_receipt_path),
            "python_wheel_validation": sha256(wheel_evidence_path),
            "documentation_validation": sha256(docs_evidence_path),
            "public_surface_validation": sha256(publication_evidence_path),
            "public_artifact_validation": sha256(
                artifact_publication_evidence_path
            ),
        },
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", required=True, type=Path)
    parser.add_argument("--version", required=True)
    parser.add_argument("--python-release", required=True, type=Path)
    parser.add_argument("--conformance", required=True, type=Path)
    parser.add_argument("--static-evidence", required=True, type=Path)
    parser.add_argument("--static-receipt", required=True, type=Path)
    parser.add_argument("--static-archive", required=True, type=Path)
    parser.add_argument("--static-checksum", required=True, type=Path)
    parser.add_argument("--wheel-receipt", required=True, type=Path)
    parser.add_argument("--wheel-evidence", required=True, type=Path)
    parser.add_argument("--wheel-directory", required=True, type=Path)
    parser.add_argument("--docs-evidence", required=True, type=Path)
    parser.add_argument("--publication-evidence", required=True, type=Path)
    parser.add_argument(
        "--artifact-publication-evidence", required=True, type=Path
    )
    parser.add_argument("--docs-site", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    try:
        root = args.root.resolve(strict=True)
        report = close_candidate(
            root=root,
            version=args.version,
            python_release_path=args.python_release.resolve(strict=True),
            conformance_path=args.conformance.resolve(strict=True),
            static_evidence_path=args.static_evidence.resolve(strict=True),
            static_receipt_path=args.static_receipt.resolve(strict=True),
            static_archive_path=args.static_archive,
            static_checksum_path=args.static_checksum,
            wheel_receipt_path=args.wheel_receipt.resolve(strict=True),
            wheel_evidence_path=args.wheel_evidence.resolve(strict=True),
            wheel_directory=args.wheel_directory.resolve(strict=True),
            docs_evidence_path=args.docs_evidence.resolve(strict=True),
            publication_evidence_path=args.publication_evidence.resolve(strict=True),
            artifact_publication_evidence_path=(
                args.artifact_publication_evidence.resolve(strict=True)
            ),
            docs_site=args.docs_site,
        )
        write_json(args.output.resolve(), report)
    except (OSError, ValueError, ReleaseCandidateError) as error:
        parser.error(str(error))
    print(
        "local release candidate: pass "
        "(tested static SDK, Python wheel, documentation, conformance)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
