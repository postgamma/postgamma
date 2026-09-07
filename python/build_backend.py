"""Dependency-free PEP 517 backend for the self-contained PostGamma wheel."""

from __future__ import annotations

import argparse
import base64
import csv
import hashlib
import io
import json
import os
import re
import shlex
import shutil
import subprocess
import sys
import sysconfig
import tempfile
import zipfile
from pathlib import Path
from typing import Any, Iterable


NAME = "postgamma"
KERNEL_ARCHIVE_NAME = "libpostgamma_python_abi1.a"
STATIC_KERNEL_MARKER = "postgamma-static-kernel.json"
STATIC_LINK_OPTION = re.compile(r"^(?:-pthread|-l[A-Za-z0-9_+.-]+)$")
ROOT = Path(__file__).resolve().parent.parent
VERSION_FILE = ROOT / "VERSION"
PACKAGE_SOURCE = Path(__file__).resolve().parent / "src" / NAME
PYTHON_ROOT = Path(__file__).resolve().parent
README_SOURCE = PYTHON_ROOT / "README.md"
LICENSE_SOURCES = (
    ("LICENSE", ROOT / "LICENSE"),
    ("NOTICE", ROOT / "NOTICE"),
    ("THIRD_PARTY_NOTICES", ROOT / "THIRD_PARTY_NOTICES"),
    ("LICENSE.postgresql", ROOT / "licenses" / "LICENSE.postgresql"),
    ("LICENSE.pgvector", ROOT / "licenses" / "LICENSE.pgvector"),
)


def _receipt_license_files() -> list[str]:
    """Return the canonical license identity recorded by build receipts."""

    return sorted(name for name, _path in LICENSE_SOURCES)
LICENSE_EXPRESSION = "Apache-2.0"
METADATA_VERSION = "2.4"
SUMMARY = "PostgreSQL embedded directly in a Python process"
REQUIRES_PYTHON = ">=3.10"
DESCRIPTION_CONTENT_TYPE = "text/markdown"
PROJECT_URL = "https://postgamma.com"
NATIVE_SOURCES = (
    "_native.c",
    "_native_requests.c",
    "_native_operations.c",
    "_native_arrow.c",
)


def _distribution_version() -> str:
    value = VERSION_FILE.read_text(encoding="ascii")
    if not re.fullmatch(
        r"(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\."
        r"(?:0|[1-9][0-9]*)(?:(?:a|b|rc)[1-9][0-9]*)?\n",
        value,
    ):
        raise RuntimeError("VERSION does not contain a supported public version")
    return value[:-1]


VERSION = _distribution_version()
DIST_INFO = f"{NAME}-{VERSION}.dist-info"


def _development_status() -> str:
    if "a" in VERSION:
        return "3 - Alpha"
    if "b" in VERSION or "rc" in VERSION or VERSION.startswith("0."):
        return "4 - Beta"
    return "5 - Production/Stable"


def _platform_tag() -> str:
    return sysconfig.get_platform().replace("-", "_").replace(".", "_")


def _python_tag() -> tuple[str, str]:
    implementation = sys.implementation.name
    if implementation != "cpython":
        raise RuntimeError("PostGamma binary wheels currently require CPython")
    tag = f"cp{sys.version_info.major}{sys.version_info.minor}"
    return tag, tag + getattr(sys, "abiflags", "")


def _wheel_tag() -> str:
    python, abi = _python_tag()
    return f"{python}-{abi}-{_platform_tag()}"


def _metadata() -> bytes:
    description = README_SOURCE.read_text(encoding="utf-8")
    headers = [
        f"Metadata-Version: {METADATA_VERSION}",
        f"Name: {NAME}",
        f"Version: {VERSION}",
        f"Summary: {SUMMARY}",
        f"Requires-Python: {REQUIRES_PYTHON}",
        "Author: PostGamma contributors",
        f"License-Expression: {LICENSE_EXPRESSION}",
        f"Project-URL: Homepage, {PROJECT_URL}",
        f"Description-Content-Type: {DESCRIPTION_CONTENT_TYPE}",
        "Keywords: embedded,database,postgresql",
        f"Classifier: Development Status :: {_development_status()}",
        "Classifier: Intended Audience :: Developers",
        "Classifier: Operating System :: POSIX :: Linux",
        "Classifier: Programming Language :: Python :: 3",
        "Classifier: Programming Language :: Python :: 3.10",
        "Classifier: Programming Language :: Python :: 3.11",
        "Classifier: Programming Language :: Python :: 3.12",
        "Classifier: Programming Language :: Python :: 3.13",
        "Classifier: Programming Language :: Python :: 3.14",
        "Classifier: Programming Language :: Python :: Implementation :: CPython",
        "Classifier: Programming Language :: C",
        "Classifier: Topic :: Database :: Database Engines/Servers",
        "Classifier: Typing :: Typed",
        *(f"License-File: {name}" for name, _path in LICENSE_SOURCES),
    ]
    return ("\n".join(headers) + "\n\n" + description).encode("utf-8")


def _wheel_metadata() -> bytes:
    return (
        "Wheel-Version: 1.0\n"
        "Generator: postgamma.build_backend\n"
        "Root-Is-Purelib: false\n"
        f"Tag: {_wheel_tag()}\n"
        "\n"
    ).encode("utf-8")


def _project_path(environment_name: str, default: Path) -> Path:
    value = os.environ.get(environment_name)
    path = Path(value).expanduser() if value else default
    try:
        return path.resolve(strict=True)
    except FileNotFoundError as error:
        raise RuntimeError(f"{environment_name} path does not exist: {path}") from error


def _copy_python_sources(package: Path) -> None:
    package.mkdir(parents=True)
    for source in sorted(PACKAGE_SOURCE.rglob("*")):
        relative = source.relative_to(PACKAGE_SOURCE)
        if (
            source.name.startswith("_native")
            and source.suffix in {".c", ".h"}
        ) or "__pycache__" in relative.parts:
            continue
        if source.is_file():
            target = package / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, target)


def _static_kernel() -> tuple[Path, list[str], str]:
    default_root = ROOT / "build" / "python-product" / "kernel-build"
    library = _project_path(
        "POSTGAMMA_STATIC_LIBRARY",
        default_root / "embedded-kernel" / "lib" / KERNEL_ARCHIVE_NAME,
    )
    options_file = _project_path(
        "POSTGAMMA_STATIC_LINK_OPTIONS",
        default_root / "embedded-kernel" / "lib" / "postgamma-static-libs.txt",
    )
    receipt_file = _project_path(
        "POSTGAMMA_STATIC_RECEIPT",
        default_root / "embedded-kernel" / "reports" / "static-library-link.json",
    )
    if not library.is_file():
        raise RuntimeError(f"POSTGAMMA_STATIC_LIBRARY is not a file: {library}")
    if not options_file.is_file():
        raise RuntimeError(
            f"POSTGAMMA_STATIC_LINK_OPTIONS is not a file: {options_file}"
        )
    if not receipt_file.is_file():
        raise RuntimeError(f"POSTGAMMA_STATIC_RECEIPT is not a file: {receipt_file}")
    try:
        options = shlex.split(options_file.read_text(encoding="utf-8"))
    except (OSError, ValueError) as error:
        raise RuntimeError(f"invalid static link options: {error}") from error
    if not options or not all(STATIC_LINK_OPTION.fullmatch(value) for value in options):
        raise RuntimeError("static link options contain an unsupported argument")
    try:
        receipt = json.loads(receipt_file.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise RuntimeError(f"invalid static-library receipt: {error}") from error
    library_digest = hashlib.sha256(library.read_bytes()).hexdigest()
    if (
        not isinstance(receipt, dict)
        or receipt.get("schema_version") != 1
        or receipt.get("kind") != "postgamma.static-library-build"
        or receipt.get("status") != "pass"
        or receipt.get("linkage") != "static"
        or receipt.get("version") != "1.3"
        or receipt.get("output_sha256") != library_digest
        or receipt.get("output_size") != library.stat().st_size
        or receipt.get("public_symbol_count") != 75
        or receipt.get("consumer_link_options") != options
        or receipt.get("determinism")
        != {"build_count": 2, "identical": True}
    ):
        raise RuntimeError("static-library receipt does not match the kernel archive")
    return library, options, library_digest


def _copy_runtime(package: Path) -> Path:
    resource_root = _project_path(
        "POSTGAMMA_RESOURCE_ROOT",
        ROOT / "build" / "embedded-release" / "resource-pack",
    )
    if not (resource_root / "bin" / "postgres").is_file() or not (
        resource_root / "share" / "postgres.bki"
    ).is_file():
        raise RuntimeError(f"invalid POSTGAMMA_RESOURCE_ROOT: {resource_root}")
    resources = package / "_resources"
    shutil.copytree(resource_root, resources, symlinks=False)
    marker = resources / "lib" / STATIC_KERNEL_MARKER
    marker.parent.mkdir(parents=True, exist_ok=True)
    marker.write_text(
        json.dumps(
            {
                "schema_version": 1,
                "kernel_linkage": "static",
                "separate_library": False,
            },
            sort_keys=True,
        )
        + "\n",
        encoding="utf-8",
    )
    return resources


def _compile_extension(
    package: Path, static_library: Path, link_options: list[str]
) -> Path:
    include_python = sysconfig.get_config_var("INCLUDEPY")
    extension_suffix = sysconfig.get_config_var("EXT_SUFFIX")
    compiler = sysconfig.get_config_var("CC") or "cc"
    if not include_python or not extension_suffix:
        raise RuntimeError(
            "the active Python installation has no extension build metadata"
        )
    python_header = Path(include_python) / "Python.h"
    postgamma_header = ROOT / "embedded-c" / "include" / "postgamma" / "postgamma.h"
    if not python_header.is_file():
        raise RuntimeError(f"Python development header is missing: {python_header}")
    if not postgamma_header.is_file():
        raise RuntimeError(f"postgamma C header is missing: {postgamma_header}")
    output = package / f"_native{extension_suffix}"
    command = [
        *shlex.split(compiler),
        "-std=c11",
        "-O2",
        "-fPIC",
        "-fvisibility=hidden",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-Wpedantic",
        f"-ffile-prefix-map={ROOT}=.",
        f"-fmacro-prefix-map={ROOT}=.",
        "-pthread",
        "-shared",
        f"-I{include_python}",
        f"-I{ROOT / 'embedded-c' / 'include'}",
        *(str(PACKAGE_SOURCE / source) for source in NATIVE_SOURCES),
        str(static_library),
        "-Wl,--exclude-libs,ALL",
        *link_options,
        "-o",
        str(output),
    ]
    result = subprocess.run(command, check=False, text=True, capture_output=True)
    if result.returncode:
        detail = result.stderr.strip() or result.stdout.strip()
        raise RuntimeError(
            f"could not compile the postgamma Python extension: {detail}"
        )
    return output


def _files(root: Path) -> Iterable[Path]:
    return sorted(path for path in root.rglob("*") if path.is_file())


def _record_digest(content: bytes) -> str:
    digest = base64.urlsafe_b64encode(hashlib.sha256(content).digest()).rstrip(b"=")
    return "sha256=" + digest.decode("ascii")


def _zip_info(name: str, mode: int) -> zipfile.ZipInfo:
    info = zipfile.ZipInfo(name, date_time=(1980, 1, 1, 0, 0, 0))
    info.compress_type = zipfile.ZIP_DEFLATED
    info.external_attr = (mode & 0xFFFF) << 16
    info.create_system = 3
    return info


def _build_archive(stage: Path, wheel_path: Path) -> None:
    records: list[tuple[str, str, str]] = []
    record_name = f"{DIST_INFO}/RECORD"
    with zipfile.ZipFile(
        wheel_path, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9
    ) as archive:
        for path in _files(stage):
            relative = path.relative_to(stage).as_posix()
            if relative == record_name:
                continue
            content = path.read_bytes()
            mode = path.stat().st_mode & 0o777
            archive.writestr(_zip_info(relative, mode), content)
            records.append((relative, _record_digest(content), str(len(content))))
        records.append((record_name, "", ""))
        buffer = io.StringIO(newline="")
        writer = csv.writer(buffer, lineterminator="\n")
        writer.writerows(records)
        archive.writestr(
            _zip_info(record_name, 0o644), buffer.getvalue().encode("utf-8")
        )


def _stage_metadata(dist_info: Path) -> None:
    dist_info.mkdir(parents=True, exist_ok=True)
    (dist_info / "METADATA").write_bytes(_metadata())
    (dist_info / "WHEEL").write_bytes(_wheel_metadata())
    (dist_info / "top_level.txt").write_text(NAME + "\n", encoding="utf-8")
    licenses = dist_info / "licenses"
    licenses.mkdir(exist_ok=True)
    for name, source in LICENSE_SOURCES:
        shutil.copy2(source, licenses / name)


def _stage_wheel(root: Path) -> None:
    package = root / NAME
    _copy_python_sources(package)
    library, link_options, _library_digest = _static_kernel()
    _copy_runtime(package)
    _compile_extension(package, library, link_options)
    _stage_metadata(root / DIST_INFO)


def get_requires_for_build_wheel(
    config_settings: dict[str, Any] | None = None,
) -> list[str]:
    del config_settings
    return []


def prepare_metadata_for_build_wheel(
    metadata_directory: str,
    config_settings: dict[str, Any] | None = None,
) -> str:
    del config_settings
    target = Path(metadata_directory) / DIST_INFO
    _stage_metadata(target)
    return DIST_INFO


def build_wheel(
    wheel_directory: str,
    config_settings: dict[str, Any] | None = None,
    metadata_directory: str | None = None,
) -> str:
    del config_settings, metadata_directory
    destination = Path(wheel_directory).resolve()
    destination.mkdir(parents=True, exist_ok=True)
    filename = f"{NAME}-{VERSION}-{_wheel_tag()}.whl"
    wheel_path = destination / filename
    with tempfile.TemporaryDirectory(
        prefix=".postgamma-wheel-", dir=destination
    ) as temporary:
        stage = Path(temporary) / "stage"
        stage.mkdir()
        _stage_wheel(stage)
        staged_wheel = Path(temporary) / filename
        _build_archive(stage, staged_wheel)
        os.replace(staged_wheel, wheel_path)
    return filename


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--wheel-dir", required=True, type=Path)
    parser.add_argument("--receipt", type=Path)
    args = parser.parse_args()
    filename = build_wheel(str(args.wheel_dir))
    wheel = args.wheel_dir.resolve() / filename
    if args.receipt is not None:
        content = wheel.read_bytes()
        _library, _link_options, kernel_digest = _static_kernel()
        document = {
            "schema_version": 2,
            "kind": "postgamma.python-wheel-build",
            "name": NAME,
            "version": VERSION,
            "filename": filename,
            "sha256": hashlib.sha256(content).hexdigest(),
            "size": len(content),
            "tags": [_wheel_tag()],
            "platform_policy": "linux_native",
            "kernel_linkage": "static",
            "bundled_kernel_library": False,
            "kernel_archive_sha256": kernel_digest,
            "python": sys.version.split()[0],
            "metadata_version": METADATA_VERSION,
            "requires_python": REQUIRES_PYTHON,
            "project_url": PROJECT_URL,
            "license_files": _receipt_license_files(),
        }
        args.receipt.parent.mkdir(parents=True, exist_ok=True)
        args.receipt.write_text(
            json.dumps(document, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
    print(wheel)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
