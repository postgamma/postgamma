#!/usr/bin/env python3
"""Check release artifacts for build-host source paths."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import tarfile
import zipfile
from pathlib import Path
from typing import BinaryIO, Iterable


EVIDENCE_KIND = "postgamma.public-artifact-evidence"
PRINTABLE = re.compile(rb"[\x20-\x7e]{8,}")
ABSOLUTE_PROJECT_PATH = re.compile(
    rb"(?i)(?<![a-z0-9_.-])(?:/[a-z0-9_.@+-]+){2,}/"
    rb"(?:postgamma(?:-embedded)?|workspace)"
    rb"(?:/|$)"
)


class PublicArtifactError(RuntimeError):
    """A release payload contains a build-host source path."""


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def preview(value: bytes) -> str:
    text = value.decode("ascii", errors="replace")
    return text if len(text) <= 160 else text[:157] + "..."


def scan_value(value: bytes, *, root: bytes, location: str) -> list[str]:
    failures: list[str] = []
    if root in value or ABSOLUTE_PROJECT_PATH.search(value):
        failures.append(f"{location}: build-host source path: {preview(value)}")
    return failures


def scan_content(
    content: bytes, *, root: bytes, location: str, limit: int
) -> list[str]:
    failures: list[str] = []
    for match in PRINTABLE.finditer(content):
        failures.extend(scan_value(match.group(), root=root, location=location))
        if len(failures) >= limit:
            break
    return failures


def scan_entries(
    entries: Iterable[tuple[str, BinaryIO]], *, root: bytes, limit: int = 20
) -> tuple[int, list[str]]:
    failures: list[str] = []
    count = 0
    for name, stream in entries:
        count += 1
        failures.extend(scan_value(name.encode("utf-8"), root=root, location=name))
        if len(failures) < limit:
            failures.extend(
                scan_content(
                    stream.read(), root=root, location=name, limit=limit - len(failures)
                )
            )
        if len(failures) >= limit:
            break
    return count, failures


def tar_entries(archive: tarfile.TarFile) -> Iterable[tuple[str, BinaryIO]]:
    for member in archive.getmembers():
        if not member.isfile():
            continue
        stream = archive.extractfile(member)
        if stream is None:
            raise PublicArtifactError(f"cannot read static SDK member {member.name}")
        with stream:
            yield member.name, stream


def zip_entries(archive: zipfile.ZipFile) -> Iterable[tuple[str, BinaryIO]]:
    for member in archive.infolist():
        if member.is_dir():
            continue
        with archive.open(member, "r") as stream:
            yield member.filename, stream


def check(root: Path, static_archive: Path, wheel: Path) -> dict[str, object]:
    source_root = str(root.resolve(strict=True)).encode("utf-8")
    static_archive = static_archive.resolve(strict=True)
    wheel = wheel.resolve(strict=True)
    try:
        with tarfile.open(static_archive, "r:gz") as archive:
            static_files, static_failures = scan_entries(
                tar_entries(archive), root=source_root
            )
    except (tarfile.TarError, OSError) as error:
        raise PublicArtifactError(f"cannot inspect static SDK archive: {error}") from error
    try:
        with zipfile.ZipFile(wheel, "r") as archive:
            wheel_files, wheel_failures = scan_entries(
                zip_entries(archive), root=source_root
            )
    except (zipfile.BadZipFile, OSError) as error:
        raise PublicArtifactError(f"cannot inspect Python wheel: {error}") from error
    failures = static_failures + wheel_failures
    if failures:
        raise PublicArtifactError(
            "release artifacts contain build-host source paths:\n  "
            + "\n  ".join(failures)
        )
    return {
        "schema_version": 1,
        "kind": EVIDENCE_KIND,
        "status": "pass",
        "build_path_violations": 0,
        "static_sdk": {
            "sha256": sha256(static_archive),
            "scanned_file_count": static_files,
        },
        "python_wheel": {
            "sha256": sha256(wheel),
            "scanned_file_count": wheel_files,
        },
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", required=True, type=Path)
    parser.add_argument("--static-archive", required=True, type=Path)
    parser.add_argument("--wheel-receipt", required=True, type=Path)
    parser.add_argument("--wheel-directory", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    try:
        receipt = json.loads(args.wheel_receipt.read_text(encoding="utf-8"))
        wheel_name = receipt.get("filename") if isinstance(receipt, dict) else None
        if not isinstance(wheel_name, str) or Path(wheel_name).name != wheel_name:
            raise PublicArtifactError("Python wheel receipt has no safe filename")
        report = check(
            args.root,
            args.static_archive,
            args.wheel_directory / wheel_name,
        )
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(
            json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8"
        )
    except (OSError, UnicodeError, json.JSONDecodeError, PublicArtifactError) as error:
        parser.error(str(error))
    print("release artifacts: pass")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
