#!/usr/bin/env python3
"""Promote tested build outputs into one immutable release asset set."""

from __future__ import annotations

import argparse
import hashlib
import json
import shutil
from pathlib import Path
from typing import Any

from product_version import ProductVersionError, read_version

MANIFEST_KIND = "postgamma.release-assets"
PYTHON_EVIDENCE_KIND = "postgamma.python-release-evidence"
STATIC_RECEIPT_KIND = "postgamma.static-sdk-archive"
LICENSE_ASSETS = (
    "LICENSE",
    "NOTICE",
    "THIRD_PARTY_NOTICES",
    "licenses/LICENSE.postgresql",
    "licenses/LICENSE.pgvector",
)


class ReleaseAssetError(RuntimeError):
    """A release asset is missing, stale, duplicated, or untested."""


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def load_json(path: Path) -> dict[str, Any]:
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise ReleaseAssetError(f"cannot read {path}: {error}") from error
    if not isinstance(document, dict):
        raise ReleaseAssetError(f"{path} must contain a JSON object")
    return document


def require_passing(document: dict[str, Any], kind: str, label: str) -> None:
    if document.get("kind") != kind or document.get("status") != "pass":
        raise ReleaseAssetError(f"{label} is not passing {kind} evidence")


def require_empty_directory(path: Path) -> None:
    if path.exists():
        if not path.is_dir() or any(path.iterdir()):
            raise ReleaseAssetError(
                f"release output must be an empty directory: {path}"
            )
    else:
        path.mkdir(parents=True)


def unique_file(root: Path, filename: str) -> Path:
    if Path(filename).name != filename:
        raise ReleaseAssetError(f"release filename is not a basename: {filename!r}")
    matches = sorted(path for path in root.rglob(filename) if path.is_file())
    if len(matches) != 1:
        raise ReleaseAssetError(
            f"expected one tested {filename}, found {len(matches)} below {root}"
        )
    return matches[0]


def copy_verified(
    source: Path,
    output: Path,
    expected_digest: str,
    role: str,
) -> dict[str, Any]:
    actual = sha256(source)
    if actual != expected_digest:
        raise ReleaseAssetError(f"tested digest differs for {source.name}")
    destination = output / source.name
    if destination.exists():
        raise ReleaseAssetError(f"duplicate release asset: {destination.name}")
    shutil.copy2(source, destination)
    if sha256(destination) != expected_digest:
        raise ReleaseAssetError(f"release copy changed bytes: {destination.name}")
    return {
        "filename": destination.name,
        "role": role,
        "sha256": expected_digest,
        "size": destination.stat().st_size,
    }


def prepare(
    *,
    root: Path,
    python_artifacts: Path,
    python_evidence_path: Path,
    static_archive: Path,
    static_checksum: Path,
    static_receipt_path: Path,
    output: Path,
) -> dict[str, Any]:
    version = read_version(root)
    require_empty_directory(output)

    python_evidence = load_json(python_evidence_path)
    require_passing(python_evidence, PYTHON_EVIDENCE_KIND, "Python matrix")
    if (
        python_evidence.get("release", {}).get("version") != version.text
        or python_evidence.get("public_release_ready") is not True
        or python_evidence.get("distribution_matrix_verified") is not True
        or python_evidence.get("publish_authorized") is not False
    ):
        raise ReleaseAssetError("Python matrix is not a sealed release input")
    wheels = python_evidence.get("wheels")
    if (
        not isinstance(wheels, list)
        or len(wheels) != python_evidence.get("wheel_count")
        or not wheels
    ):
        raise ReleaseAssetError("Python matrix has no exact wheel inventory")

    assets: list[dict[str, Any]] = []
    wheel_names: set[str] = set()
    for wheel in wheels:
        if not isinstance(wheel, dict):
            raise ReleaseAssetError("Python wheel inventory entry is invalid")
        filename = wheel.get("filename")
        digest = wheel.get("sha256")
        if (
            not isinstance(filename, str)
            or not filename.endswith(".whl")
            or f"-{version.text}-" not in filename
            or filename in wheel_names
            or not isinstance(digest, str)
        ):
            raise ReleaseAssetError("Python wheel identity is invalid")
        wheel_names.add(filename)
        assets.append(
            copy_verified(
                unique_file(python_artifacts, filename),
                output,
                digest,
                "python-wheel",
            )
        )

    static_receipt = load_json(static_receipt_path)
    require_passing(static_receipt, STATIC_RECEIPT_KIND, "static SDK")
    static_digest = static_receipt.get("archive_sha256")
    if (
        static_receipt.get("product_version") != version.text
        or static_receipt.get("archive") != static_archive.name
        or static_receipt.get("checksum") != static_checksum.name
        or not isinstance(static_digest, str)
    ):
        raise ReleaseAssetError("static SDK identity differs from VERSION")
    expected_checksum = f"{static_digest}  {static_archive.name}\n"
    if static_checksum.read_text(encoding="ascii") != expected_checksum:
        raise ReleaseAssetError("static SDK checksum file is not canonical")
    assets.append(copy_verified(static_archive, output, static_digest, "static-sdk"))
    assets.append(
        copy_verified(
            static_checksum,
            output,
            sha256(static_checksum),
            "static-sdk-checksum",
        )
    )

    for relative in LICENSE_ASSETS:
        source = root / relative
        assets.append(copy_verified(source, output, sha256(source), "license-notice"))

    assets.sort(key=lambda item: item["filename"])
    manifest = {
        "schema_version": 1,
        "kind": MANIFEST_KIND,
        "status": "pass",
        "release": {
            "name": "postgamma",
            "version": version.text,
            "tag": version.tag,
            "state": version.state,
            "prerelease": version.prerelease,
            "postgresql_major": python_evidence.get("postgresql", {}).get("major"),
        },
        "assets": assets,
        "evidence": {
            "python_matrix_sha256": sha256(python_evidence_path),
            "static_sdk_receipt_sha256": sha256(static_receipt_path),
        },
        "promotion": {
            "rebuilt_after_test": False,
            "exact_tested_bytes": True,
        },
    }
    manifest_path = output / "release-manifest.json"
    manifest_path.write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    checksum_paths = sorted(
        [output / item["filename"] for item in assets] + [manifest_path],
        key=lambda path: path.name,
    )
    (output / "SHA256SUMS").write_text(
        "".join(f"{sha256(path)}  {path.name}\n" for path in checksum_paths),
        encoding="ascii",
    )
    return manifest


def checksum_inventory(path: Path) -> dict[str, str]:
    entries: dict[str, str] = {}
    for line in path.read_text(encoding="ascii").splitlines():
        fields = line.split("  ", 1)
        if (
            len(fields) != 2
            or len(fields[0]) != 64
            or any(character not in "0123456789abcdef" for character in fields[0])
            or Path(fields[1]).name != fields[1]
            or fields[1] in entries
        ):
            raise ReleaseAssetError("SHA256SUMS contains an invalid entry")
        entries[fields[1]] = fields[0]
    return entries


def verify_wheels(root: Path, bundle: Path, tag: str) -> dict[str, Any]:
    version = read_version(root)
    if tag != version.tag:
        raise ReleaseAssetError(f"release tag {tag!r} differs from {version.tag!r}")
    manifest_path = bundle / "release-manifest.json"
    sums = checksum_inventory(bundle / "SHA256SUMS")
    if sums.get(manifest_path.name) != sha256(manifest_path):
        raise ReleaseAssetError("release manifest checksum differs")
    manifest = load_json(manifest_path)
    if (
        manifest.get("kind") != MANIFEST_KIND
        or manifest.get("status") != "pass"
        or manifest.get("release", {}).get("version") != version.text
        or manifest.get("release", {}).get("tag") != tag
        or manifest.get("promotion", {}).get("exact_tested_bytes") is not True
    ):
        raise ReleaseAssetError("release manifest identity differs from VERSION")
    entries = manifest.get("assets")
    if not isinstance(entries, list) or not all(
        isinstance(entry, dict) for entry in entries
    ):
        raise ReleaseAssetError("release manifest has no asset inventory")
    wheels = [entry for entry in entries if entry.get("role") == "python-wheel"]
    if not all(
        isinstance(entry.get("filename"), str)
        and Path(entry["filename"]).name == entry["filename"]
        and isinstance(entry.get("sha256"), str)
        for entry in wheels
    ):
        raise ReleaseAssetError("release manifest has an invalid wheel entry")
    expected = {entry["filename"] for entry in wheels}
    present = {path.name for path in bundle.glob("*.whl") if path.is_file()}
    if not wheels or len(expected) != len(wheels) or expected != present:
        raise ReleaseAssetError(
            "downloaded wheel set differs from the release manifest"
        )
    for entry in wheels:
        path = bundle / entry["filename"]
        digest = sha256(path)
        if digest != entry.get("sha256") or digest != sums.get(path.name):
            raise ReleaseAssetError(f"release wheel checksum differs: {path.name}")
    return {
        "version": version.text,
        "tag": tag,
        "wheel_count": len(wheels),
        "verified": True,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    subparsers = parser.add_subparsers(dest="command", required=True)
    prepare_parser = subparsers.add_parser("prepare")
    prepare_parser.add_argument("--root", required=True, type=Path)
    prepare_parser.add_argument("--python-artifacts", required=True, type=Path)
    prepare_parser.add_argument("--python-evidence", required=True, type=Path)
    prepare_parser.add_argument("--static-archive", required=True, type=Path)
    prepare_parser.add_argument("--static-checksum", required=True, type=Path)
    prepare_parser.add_argument("--static-receipt", required=True, type=Path)
    prepare_parser.add_argument("--output", required=True, type=Path)
    verify_parser = subparsers.add_parser("verify-wheels")
    verify_parser.add_argument("--root", required=True, type=Path)
    verify_parser.add_argument("--bundle", required=True, type=Path)
    verify_parser.add_argument("--tag", required=True)
    args = parser.parse_args()
    try:
        if args.command == "prepare":
            report = prepare(
                root=args.root.resolve(),
                python_artifacts=args.python_artifacts.resolve(),
                python_evidence_path=args.python_evidence.resolve(),
                static_archive=args.static_archive.resolve(),
                static_checksum=args.static_checksum.resolve(),
                static_receipt_path=args.static_receipt.resolve(),
                output=args.output.resolve(),
            )
            print(f"release assets: {len(report['assets'])} tested payloads prepared")
        else:
            report = verify_wheels(args.root.resolve(), args.bundle.resolve(), args.tag)
            print(f"release wheels: {report['wheel_count']} exact assets verified")
    except (OSError, ProductVersionError, ReleaseAssetError) as error:
        parser.error(str(error))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
