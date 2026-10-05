#!/usr/bin/env python3
"""Build a deterministic, relocatable PostGamma static SDK release archive."""

from __future__ import annotations

import argparse
import gzip
import hashlib
import io
import json
import os
import re
import tarfile
import tempfile
from dataclasses import dataclass
from pathlib import Path, PurePosixPath
from typing import Iterable


ARCHIVE_KIND = "postgamma.static-sdk-archive"
MANIFEST_KIND = "postgamma.static-sdk-manifest"
PROJECT_LICENSE_EXPRESSION = "Apache-2.0"
PROJECT_URL = "https://postgamma.com"
PROJECT_LICENSE_SHA256 = (
    "cfc7749b96f63bd31c3c42b5c471bf756814053e847c10f3eb003417bc523d30"
)
PROJECT_NOTICE_SHA256 = (
    "db5e2854154f8d331b970cf94093706f8d87b58fcaad01b990256e7536cdc61e"
)
THIRD_PARTY_NOTICES_SHA256 = (
    "b5107ecdb41860f5a8c7381083520fe10311939a74da7a79f4899bdcc1a7a876"
)
POSTGRESQL_LICENSE_SHA256 = (
    "3d6af92ff8a4c2cdf69afb1cf44edea727922f5cd0cf8b5f72b11cdecac8fdfd"
)
PGVECTOR_LICENSE_SHA256 = (
    "6bba9ebeb73e27477463b05e5ef1bf303bccbddb3db9bbc95905d351604d6a87"
)
SAFE_COMPONENT = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]*$")
REQUIRED_SDK_FILES = (
    "include/postgamma/postgamma.h",
    "include/postgamma/postgamma_arrow.h",
    "include/postgamma/postgamma_extension.h",
    "lib/libpostgamma.a",
    "lib/postgamma-static-libs.txt",
    "lib/pkgconfig/postgamma.pc",
)


class StaticSdkPackageError(RuntimeError):
    """The public static SDK release archive is incomplete or unsafe."""


@dataclass(frozen=True)
class PayloadFile:
    logical_path: PurePosixPath
    source: Path
    mode: int
    size: int
    sha256: str


def file_sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def normalized_mode(path: Path) -> int:
    return 0o755 if path.stat().st_mode & 0o111 else 0o644


def require_component(name: str, value: str) -> str:
    if not SAFE_COMPONENT.fullmatch(value):
        raise StaticSdkPackageError(
            f"{name} must contain only letters, digits, '.', '_', and '-': {value!r}"
        )
    return value


def require_regular_file(path: Path, description: str) -> Path:
    try:
        resolved = path.resolve(strict=True)
    except FileNotFoundError as error:
        raise StaticSdkPackageError(f"{description} is missing: {path}") from error
    if path.is_symlink() or not resolved.is_file():
        raise StaticSdkPackageError(
            f"{description} must be a regular, non-symlink file: {path}"
        )
    return resolved


def require_directory(path: Path, description: str) -> Path:
    try:
        resolved = path.resolve(strict=True)
    except FileNotFoundError as error:
        raise StaticSdkPackageError(f"{description} is missing: {path}") from error
    if path.is_symlink() or not resolved.is_dir():
        raise StaticSdkPackageError(
            f"{description} must be a non-symlink directory: {path}"
        )
    return resolved


def payload_file(logical_path: str, source: Path) -> PayloadFile:
    path = PurePosixPath(logical_path)
    if path.is_absolute() or ".." in path.parts or "." in path.parts:
        raise StaticSdkPackageError(f"unsafe archive path: {logical_path}")
    resolved = require_regular_file(source, logical_path)
    return PayloadFile(
        logical_path=path,
        source=resolved,
        mode=normalized_mode(resolved),
        size=resolved.stat().st_size,
        sha256=file_sha256(resolved),
    )


def resource_entries(resource_root: Path) -> tuple[list[PurePosixPath], list[PayloadFile]]:
    root = require_directory(resource_root, "resource pack")
    directories: list[PurePosixPath] = [PurePosixPath("resource-pack")]
    files: list[PayloadFile] = []
    for source in sorted(root.rglob("*")):
        relative = source.relative_to(root)
        logical = PurePosixPath("resource-pack", *relative.parts)
        if source.is_symlink():
            raise StaticSdkPackageError(
                f"resource pack may not contain symlinks: {relative.as_posix()}"
            )
        if source.is_dir():
            directories.append(logical)
        elif source.is_file():
            files.append(payload_file(logical.as_posix(), source))
        else:
            raise StaticSdkPackageError(
                f"resource pack contains a non-regular entry: {relative.as_posix()}"
            )
    required = (root / "bin/postgres", root / "share/postgres.bki")
    for path in required:
        require_regular_file(path, "required resource-pack file")
    return directories, files


def sdk_abi_version(pkg_config: Path) -> str:
    matches = [
        line.removeprefix("Version:").strip()
        for line in pkg_config.read_text(encoding="utf-8").splitlines()
        if line.startswith("Version:")
    ]
    if len(matches) != 1 or not matches[0]:
        raise StaticSdkPackageError("postgamma.pc must declare exactly one Version")
    return matches[0]


def tree_sha256(files: Iterable[PayloadFile]) -> str:
    digest = hashlib.sha256()
    for entry in sorted(files, key=lambda item: item.logical_path.as_posix()):
        path = entry.logical_path.as_posix().encode("utf-8")
        digest.update(len(path).to_bytes(8, "big"))
        digest.update(path)
        digest.update(entry.mode.to_bytes(4, "big"))
        digest.update(entry.size.to_bytes(8, "big"))
        digest.update(bytes.fromhex(entry.sha256))
    return digest.hexdigest()


def manifest_bytes(
    *,
    version: str,
    platform: str,
    sdk_abi: str,
    files: list[PayloadFile],
) -> bytes:
    document = {
        "schema_version": 1,
        "kind": MANIFEST_KIND,
        "product_version": version,
        "platform": platform,
        "postgresql_major": 19,
        "sdk_abi": sdk_abi,
        "license_expression": PROJECT_LICENSE_EXPRESSION,
        "project_url": PROJECT_URL,
        "payload_tree_sha256": tree_sha256(files),
        "files": [
            {
                "path": entry.logical_path.as_posix(),
                "mode": f"{entry.mode:04o}",
                "size": entry.size,
                "sha256": entry.sha256,
            }
            for entry in sorted(files, key=lambda item: item.logical_path.as_posix())
        ],
    }
    return (json.dumps(document, indent=2, sort_keys=True) + "\n").encode("utf-8")


def tar_info(name: str, *, mode: int, size: int, epoch: int, kind: bytes) -> tarfile.TarInfo:
    info = tarfile.TarInfo(name)
    info.type = kind
    info.mode = mode
    info.size = size
    info.mtime = epoch
    info.uid = 0
    info.gid = 0
    info.uname = ""
    info.gname = ""
    return info


def archive_bytes(
    *,
    archive_root: str,
    directories: Iterable[PurePosixPath],
    files: list[PayloadFile],
    manifest: bytes,
    epoch: int,
) -> bytes:
    uncompressed = io.BytesIO()
    all_directories = {PurePosixPath(archive_root)}
    for directory in directories:
        current = PurePosixPath(archive_root) / directory
        while current != PurePosixPath("."):
            all_directories.add(current)
            if current == PurePosixPath(archive_root):
                break
            current = current.parent
    for entry in files:
        current = (PurePosixPath(archive_root) / entry.logical_path).parent
        while current != PurePosixPath("."):
            all_directories.add(current)
            if current == PurePosixPath(archive_root):
                break
            current = current.parent
    with tarfile.open(fileobj=uncompressed, mode="w", format=tarfile.PAX_FORMAT) as archive:
        for directory in sorted(all_directories, key=lambda item: item.as_posix()):
            name = directory.as_posix().rstrip("/") + "/"
            archive.addfile(
                tar_info(name, mode=0o755, size=0, epoch=epoch, kind=tarfile.DIRTYPE)
            )
        for entry in sorted(files, key=lambda item: item.logical_path.as_posix()):
            name = (PurePosixPath(archive_root) / entry.logical_path).as_posix()
            with entry.source.open("rb") as stream:
                archive.addfile(
                    tar_info(
                        name,
                        mode=entry.mode,
                        size=entry.size,
                        epoch=epoch,
                        kind=tarfile.REGTYPE,
                    ),
                    stream,
                )
        manifest_name = f"{archive_root}/MANIFEST.json"
        archive.addfile(
            tar_info(
                manifest_name,
                mode=0o644,
                size=len(manifest),
                epoch=epoch,
                kind=tarfile.REGTYPE,
            ),
            io.BytesIO(manifest),
        )
    compressed = io.BytesIO()
    with gzip.GzipFile(
        filename="", mode="wb", compresslevel=9, fileobj=compressed, mtime=epoch
    ) as stream:
        stream.write(uncompressed.getvalue())
    return compressed.getvalue()


def atomic_write(path: Path, content: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary_name = tempfile.mkstemp(
        prefix=f".{path.name}.", dir=path.parent
    )
    temporary = Path(temporary_name)
    try:
        with os.fdopen(descriptor, "wb") as stream:
            stream.write(content)
        temporary.chmod(0o644)
        os.replace(temporary, path)
    finally:
        temporary.unlink(missing_ok=True)


def package_static_sdk(
    *,
    sdk_root: Path,
    resource_root: Path,
    example: Path,
    project_license: Path,
    project_notice: Path,
    third_party_notices: Path,
    postgresql_license: Path,
    pgvector_license: Path,
    version: str,
    platform: str,
    source_date_epoch: int,
    output: Path,
    checksum: Path,
    receipt: Path,
) -> dict[str, object]:
    version = require_component("version", version)
    platform = require_component("platform", platform)
    if source_date_epoch < 0:
        raise StaticSdkPackageError("source date epoch may not be negative")
    sdk = require_directory(sdk_root, "static SDK root")
    files = [
        payload_file(relative, sdk / relative) for relative in REQUIRED_SDK_FILES
    ]
    directories, resources = resource_entries(resource_root)
    files.extend(resources)
    project_license_file = payload_file(
        "licenses/LICENSE.postgamma", project_license
    )
    if project_license_file.sha256 != PROJECT_LICENSE_SHA256:
        raise StaticSdkPackageError(
            "project license must be the canonical Apache-2.0 text"
        )
    project_notice_file = payload_file("licenses/NOTICE", project_notice)
    if project_notice_file.sha256 != PROJECT_NOTICE_SHA256:
        raise StaticSdkPackageError("project NOTICE does not match its release baseline")
    third_party_notices_file = payload_file(
        "licenses/THIRD_PARTY_NOTICES", third_party_notices
    )
    if third_party_notices_file.sha256 != THIRD_PARTY_NOTICES_SHA256:
        raise StaticSdkPackageError(
            "third-party notices do not match their release baseline"
        )
    pgvector_license_file = payload_file(
        "licenses/LICENSE.pgvector", pgvector_license
    )
    if pgvector_license_file.sha256 != PGVECTOR_LICENSE_SHA256:
        raise StaticSdkPackageError(
            "pgvector license must match the pinned upstream release"
        )
    postgresql_license_file = payload_file(
        "licenses/LICENSE.postgresql", postgresql_license
    )
    if postgresql_license_file.sha256 != POSTGRESQL_LICENSE_SHA256:
        raise StaticSdkPackageError(
            "PostgreSQL license must match the pinned upstream release"
        )
    files.extend(
        (
            payload_file("examples/quickstart.c", example),
            project_license_file,
            project_notice_file,
            third_party_notices_file,
            postgresql_license_file,
            pgvector_license_file,
        )
    )
    logical_paths = [entry.logical_path.as_posix() for entry in files]
    if len(logical_paths) != len(set(logical_paths)):
        raise StaticSdkPackageError("static SDK archive contains duplicate paths")
    sdk_abi = sdk_abi_version(sdk / "lib/pkgconfig/postgamma.pc")
    archive_root = f"postgamma-sdk-{version}-{platform}"
    manifest = manifest_bytes(
        version=version,
        platform=platform,
        sdk_abi=sdk_abi,
        files=files,
    )
    content = archive_bytes(
        archive_root=archive_root,
        directories=directories,
        files=files,
        manifest=manifest,
        epoch=source_date_epoch,
    )
    output = output.resolve()
    checksum = checksum.resolve()
    receipt = receipt.resolve()
    atomic_write(output, content)
    archive_digest = hashlib.sha256(content).hexdigest()
    atomic_write(
        checksum,
        f"{archive_digest}  {output.name}\n".encode("ascii"),
    )
    document: dict[str, object] = {
        "schema_version": 1,
        "kind": ARCHIVE_KIND,
        "status": "pass",
        "product_version": version,
        "platform": platform,
        "postgresql_major": 19,
        "sdk_abi": sdk_abi,
        "license_expression": PROJECT_LICENSE_EXPRESSION,
        "project_url": PROJECT_URL,
        "source_date_epoch": source_date_epoch,
        "archive": output.name,
        "checksum": checksum.name,
        "archive_root": archive_root,
        "archive_size": len(content),
        "archive_sha256": archive_digest,
        "payload_file_count": len(files),
        "payload_tree_sha256": tree_sha256(files),
        "static_library_sha256": file_sha256(
            sdk / "lib/libpostgamma.a"
        ),
        "project_license_sha256": file_sha256(project_license.resolve(strict=True)),
        "project_notice_sha256": file_sha256(project_notice.resolve(strict=True)),
        "third_party_notices_sha256": file_sha256(
            third_party_notices.resolve(strict=True)
        ),
        "postgresql_license_sha256": file_sha256(
            postgresql_license.resolve(strict=True)
        ),
        "pgvector_license_sha256": file_sha256(
            pgvector_license.resolve(strict=True)
        ),
    }
    atomic_write(
        receipt,
        (json.dumps(document, indent=2, sort_keys=True) + "\n").encode("utf-8"),
    )
    return document


def environment_epoch() -> int:
    value = os.environ.get("SOURCE_DATE_EPOCH", "0")
    try:
        return int(value)
    except ValueError as error:
        raise StaticSdkPackageError(
            f"SOURCE_DATE_EPOCH is not an integer: {value!r}"
        ) from error


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--sdk-root", required=True, type=Path)
    parser.add_argument("--resource-root", required=True, type=Path)
    parser.add_argument("--example", required=True, type=Path)
    parser.add_argument("--project-license", required=True, type=Path)
    parser.add_argument("--project-notice", required=True, type=Path)
    parser.add_argument("--third-party-notices", required=True, type=Path)
    parser.add_argument("--postgresql-license", required=True, type=Path)
    parser.add_argument("--pgvector-license", required=True, type=Path)
    parser.add_argument("--version", required=True)
    parser.add_argument("--platform", required=True)
    parser.add_argument("--source-date-epoch", type=int, default=environment_epoch())
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--checksum", required=True, type=Path)
    parser.add_argument("--receipt", required=True, type=Path)
    args = parser.parse_args()
    try:
        document = package_static_sdk(
            sdk_root=args.sdk_root,
            resource_root=args.resource_root,
            example=args.example,
            project_license=args.project_license,
            project_notice=args.project_notice,
            third_party_notices=args.third_party_notices,
            postgresql_license=args.postgresql_license,
            pgvector_license=args.pgvector_license,
            version=args.version,
            platform=args.platform,
            source_date_epoch=args.source_date_epoch,
            output=args.output,
            checksum=args.checksum,
            receipt=args.receipt,
        )
    except (OSError, UnicodeError, StaticSdkPackageError) as error:
        parser.error(str(error))
    print(
        "static SDK archive: "
        f"{document['archive']} ({document['payload_file_count']} files, "
        f"sha256 {document['archive_sha256']})"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
