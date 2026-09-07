#!/usr/bin/env python3
"""Validate and execute a self-contained PostGamma Python wheel."""

from __future__ import annotations

import argparse
import base64
import csv
import email.parser
import hashlib
import json
import os
import re
import shutil
import stat
import subprocess
import sys
import zipfile
from pathlib import Path, PurePosixPath
from typing import Any, Sequence

from check_documentation import python_example_manifest
from product_version import ProductVersionError, parse_version


REPORT_KIND = "postgamma.python-wheel-evidence"
BASELINE_KIND = "postgamma.python-release-baseline"
KERNEL_ARCHIVE_NAME = "libpostgamma_python_abi1.a"
NATIVE_DYNAMIC_EXPORT = "PyInit__native"
STATIC_KERNEL_MARKER = "postgamma/_resources/lib/postgamma-static-kernel.json"
SCENARIO_MARKER = "POSTGAMMA_PYTHON_SCENARIOS"
EXPECTED_SCENARIOS = {
    "test_01_library_and_topology": "runtime_closure",
    "test_02_dbapi_transactions_and_errors": "transactions_and_diagnostics",
    "test_03_typed_round_trip": "typed_round_trip",
    "test_04_connections_execute_in_parallel_without_the_gil": "gil_parallelism",
    "test_05_timeout_cancels_and_connection_is_reusable": "timeout_and_cancel",
    "test_06_cross_thread_cancel": "timeout_and_cancel",
    "test_07_inherited_handles_fail_closed_after_fork": "fork_invalidation",
    "test_08_top_level_connect_shares_process_local_instance": "shared_lifecycle",
    "test_09_unclosed_handles_warn_and_release_the_cluster": "finalizer_cleanup",
    "test_10_bounded_streaming_and_prepared_statements": "streaming_and_prepared",
    "test_11_copy_round_trip_and_early_abort": "copy_streaming",
    "test_12_notice_notification_and_event_routing": "event_routing",
    "test_13_management_and_logical_round_trip": "management_and_logical",
    "test_14_arrow_capsule_protocol": "arrow_interop",
    "test_15_asyncio_waitables_parallelism_and_cancellation": "asyncio_waitables",
    "test_16_bundled_pgvector": "bundled_pgvector",
}
ALLOWED_KERNEL_DEPENDENCIES = {
    "ld-linux-x86-64.so.2",
    "libc.so.6",
    "libdl.so.2",
    "libm.so.6",
    "libpthread.so.0",
    "librt.so.1",
    "libutil.so.1",
    "libz.so.1",
}
ALLOWED_NATIVE_DEPENDENCIES = {
    "ld-linux-x86-64.so.2",
    "libc.so.6",
    "libdl.so.2",
    "libm.so.6",
    "libpthread.so.0",
    "librt.so.1",
    "libutil.so.1",
    "libz.so.1",
}
MANYLINUX_PLATFORM_DEPENDENCIES = {
    "ld-linux-x86-64.so.2",
    "libc.so.6",
    "libdl.so.2",
    "libgcc_s.so.1",
    "libm.so.6",
    "libpthread.so.0",
    "libresolv.so.2",
    "librt.so.1",
    "libutil.so.1",
}
NEEDED = re.compile(r"Shared library: \[(?P<value>[^]]+)\]")
RUNPATH = re.compile(r"Library r(?:un)?path: \[(?P<value>[^]]*)\]", re.IGNORECASE)
SONAME = re.compile(r"Library soname: \[(?P<value>[^]]+)\]", re.IGNORECASE)
MANYLINUX = re.compile(
    r"^manylinux_(?P<major>[0-9]+)_(?P<minor>[0-9]+)_(?P<arch>.+)$"
)
MANYLINUX_ALIASES = {
    "manylinux1": (2, 5),
    "manylinux2010": (2, 12),
    "manylinux2014": (2, 17),
}


class WheelCheckError(RuntimeError):
    """The wheel does not meet the Python product contract."""


def load_json(path: Path) -> dict[str, Any]:
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise WheelCheckError(f"cannot read {path}: {error}") from error
    if not isinstance(document, dict):
        raise WheelCheckError(f"{path}: top-level value must be an object")
    return document


def release_license_files(baseline: dict[str, Any]) -> dict[str, str]:
    """Return the complete project and third-party wheel license set."""

    wheel = baseline["wheel"]
    return {**wheel["project_license"], **wheel["third_party_licenses"]}


def load_release_baseline(path: Path) -> dict[str, Any]:
    document = load_json(path)
    if document.get("schema_version") != 1 or document.get("kind") != BASELINE_KIND:
        raise WheelCheckError("invalid Python release baseline")
    release = document.get("release")
    postgresql = document.get("postgresql")
    python = document.get("python")
    wheel = document.get("wheel")
    dbapi = document.get("dbapi")
    documentation = document.get("documentation")
    exports = document.get("public_exports")
    if not all(
        isinstance(value, dict)
        for value in (release, postgresql, python, wheel, dbapi, documentation)
    ):
        raise WheelCheckError("Python release baseline has incomplete sections")
    try:
        parsed_version = parse_version(str(release.get("version", "")))
    except ProductVersionError as error:
        raise WheelCheckError(f"invalid release baseline version: {error}") from error
    if (
        release.get("name") != "postgamma"
        or release.get("state") != parsed_version.state
        or postgresql.get("major") != 19
        or postgresql.get("abi_version") != 65539
        or python.get("implementation") != "CPython"
        or wheel.get("kernel_linkage") != "static"
        or wheel.get("build_kernel_archive") != KERNEL_ARCHIVE_NAME
        or wheel.get("bundled_kernel_library") is not False
        or wheel.get("native_dynamic_export") != NATIVE_DYNAMIC_EXPORT
        or wheel.get("static_kernel_marker") != STATIC_KERNEL_MARKER
        or wheel.get("source_distribution") is not False
        or document.get("publish_authorized") is not False
        or not isinstance(documentation.get("examples"), list)
        or not documentation["examples"]
        or not all(
            isinstance(name, str) and name for name in documentation["examples"]
        )
        or len(documentation["examples"]) != len(set(documentation["examples"]))
    ):
        raise WheelCheckError("Python release baseline identity changed")
    if (
        not isinstance(exports, list)
        or not exports
        or not all(isinstance(name, str) and name for name in exports)
        or len(exports) != len(set(exports))
    ):
        raise WheelCheckError("Python public export baseline is invalid")
    project_license = wheel.get("project_license")
    third_party_licenses = wheel.get("third_party_licenses")
    project_urls = wheel.get("project_urls")
    license_sets = (project_license, third_party_licenses)
    if (
        wheel.get("license_expression") != "Apache-2.0"
        or project_urls != {"Homepage": "https://postgamma.com"}
        or not all(isinstance(licenses, dict) and licenses for licenses in license_sets)
        or not all(
            isinstance(name, str)
            and name
            and "/" not in name
            and isinstance(value, str)
            and re.fullmatch(r"[0-9a-f]{64}", value)
            for licenses in license_sets
            for name, value in licenses.items()
        )
        or set(project_license) != {"LICENSE", "NOTICE"}
        or set(third_party_licenses)
        != {"THIRD_PARTY_NOTICES", "LICENSE.postgresql", "LICENSE.pgvector"}
        or set(project_license) & set(third_party_licenses)
    ):
        raise WheelCheckError("Python license baseline is invalid")
    return document


def safe_name(name: str) -> PurePosixPath:
    path = PurePosixPath(name)
    normalized = name[:-1] if name.endswith("/") else name
    if (
        not normalized
        or "\0" in name
        or path.is_absolute()
        or ".." in path.parts
        or "\\" in name
        or PurePosixPath(normalized).as_posix() != normalized
    ):
        raise WheelCheckError(f"unsafe wheel member: {name!r}")
    return path


def validate_metadata(
    archive: zipfile.ZipFile,
    members: dict[str, zipfile.ZipInfo],
    record_name: str,
    receipt: dict[str, Any],
    wheel_name: str,
    baseline: dict[str, Any],
) -> tuple[list[str], dict[str, Any]]:
    dist_info = record_name.rsplit("/", 1)[0]
    release = baseline["release"]
    wheel_contract = baseline["wheel"]
    python_contract = baseline["python"]
    expected_dist_info = f"{release['name']}-{release['version']}.dist-info"
    if dist_info != expected_dist_info:
        raise WheelCheckError(
            f"wheel dist-info is {dist_info}, expected {expected_dist_info}"
        )
    metadata_name = f"{dist_info}/METADATA"
    wheel_metadata_name = f"{dist_info}/WHEEL"
    missing = sorted({metadata_name, wheel_metadata_name} - set(members))
    if missing:
        raise WheelCheckError(f"wheel metadata is incomplete: {missing}")
    metadata = email.parser.BytesParser().parsebytes(archive.read(metadata_name))
    if metadata.get("Name") != receipt.get("name") or metadata.get(
        "Version"
    ) != receipt.get("version"):
        raise WheelCheckError("wheel METADATA does not match the build receipt")
    expected_licenses = release_license_files(baseline)
    license_files = metadata.get_all("License-File", [])
    metadata_contract = {
        "metadata_version": metadata.get("Metadata-Version"),
        "summary": metadata.get("Summary"),
        "requires_python": metadata.get("Requires-Python"),
        "description_content_type": metadata.get("Description-Content-Type"),
        "license_expression": metadata.get("License-Expression"),
        "license_files": license_files,
        "project_urls": metadata.get_all("Project-URL", []),
    }
    expected_metadata = {
        "metadata_version": wheel_contract["metadata_version"],
        "summary": "PostgreSQL embedded directly in a Python process",
        "requires_python": python_contract["requires_python"],
        "description_content_type": wheel_contract["description_content_type"],
        "license_expression": wheel_contract["license_expression"],
        "license_files": sorted(expected_licenses),
        "project_urls": [
            f"{name}, {url}"
            for name, url in sorted(wheel_contract["project_urls"].items())
        ],
    }
    if {**metadata_contract, "license_files": sorted(license_files)} != expected_metadata:
        raise WheelCheckError("wheel core metadata differs from the release baseline")
    if not str(metadata.get_payload()).strip():
        raise WheelCheckError("wheel METADATA has no project description")
    license_members = {
        name for name in members if name.startswith(f"{dist_info}/licenses/")
    }
    expected_license_members = {
        f"{dist_info}/licenses/{name}" for name in expected_licenses
    }
    if license_members != expected_license_members:
        raise WheelCheckError("wheel license-file members differ from METADATA")
    for name, expected_hash in expected_licenses.items():
        content = archive.read(f"{dist_info}/licenses/{name}")
        if hashlib.sha256(content).hexdigest() != expected_hash:
            raise WheelCheckError(f"wheel license file changed: {name}")
    wheel_metadata = email.parser.BytesParser().parsebytes(
        archive.read(wheel_metadata_name)
    )
    tags = wheel_metadata.get_all("Tag", [])
    receipt_tags = receipt.get("tags")
    if not isinstance(receipt_tags, list) or not all(
        isinstance(tag, str) and tag for tag in receipt_tags
    ):
        raise WheelCheckError("wheel receipt has invalid tags")
    if sorted(tags) != sorted(receipt_tags):
        raise WheelCheckError(f"wheel tag does not match its receipt: {tags}")
    if sorted(filename_tags(wheel_name)) != sorted(tags):
        raise WheelCheckError("wheel filename does not match its declared tag")
    return tags, metadata_contract


def probe_public_api(
    python: str,
    site: Path,
    environment: dict[str, str],
    output: Path,
    baseline: dict[str, Any],
) -> dict[str, Any]:
    program = "\n".join(
        (
            "import json",
            "import postgamma",
            "info = postgamma.library_info()",
            "print(json.dumps({",
            "    'version': postgamma.__version__,",
            "    'exports': postgamma.__all__,",
            "    'dbapi': {",
            "        'apilevel': postgamma.apilevel,",
            "        'threadsafety': postgamma.threadsafety,",
            "        'paramstyle': postgamma.paramstyle,",
            "    },",
            "    'postgresql': {",
            "        'major': int(info['postgresql_version']),",
            "        'abi_version': int(info['abi_version']),",
            "        'build_id': info['build_id'],",
            "        'capabilities': int(postgamma.capabilities()),",
            "    },",
            "    'missing_exports': [",
            "        name for name in postgamma.__all__ if not hasattr(postgamma, name)",
            "    ],",
            "}, sort_keys=True))",
        )
    )
    result = run(
        (python, "-c", program),
        output.parent,
        environment,
        output,
    )
    try:
        document = json.loads(result.stdout)
    except json.JSONDecodeError as error:
        raise WheelCheckError(f"public API probe emitted invalid JSON: {error}") from error
    expected = {
        "version": baseline["release"]["version"],
        "exports": baseline["public_exports"],
        "dbapi": baseline["dbapi"],
        "postgresql": baseline["postgresql"],
        "missing_exports": [],
    }
    if document != expected:
        raise WheelCheckError(
            "installed Python public API differs from the release baseline"
        )
    return document


def filename_tags(wheel_name: str) -> list[str]:
    if not wheel_name.endswith(".whl"):
        raise WheelCheckError(f"invalid wheel filename: {wheel_name}")
    try:
        _prefix, python_tags, abi_tags, platform_tags = wheel_name[:-4].rsplit("-", 3)
    except ValueError as error:
        raise WheelCheckError(f"invalid wheel filename: {wheel_name}") from error
    return sorted(
        f"{python}-{abi}-{platform}"
        for python in python_tags.split(".")
        for abi in abi_tags.split(".")
        for platform in platform_tags.split(".")
    )


def require_platform_policy(tags: Sequence[str], required: str) -> None:
    required_match = MANYLINUX.fullmatch(required)
    if required_match is None:
        raise WheelCheckError(f"unsupported wheel platform policy: {required}")
    required_version = (
        int(required_match.group("major")),
        int(required_match.group("minor")),
    )
    required_arch = required_match.group("arch")
    incompatible: list[str] = []
    for tag in tags:
        try:
            platform = tag.split("-", 2)[2]
        except IndexError:
            incompatible.append(tag)
            continue
        match = MANYLINUX.fullmatch(platform)
        if match is not None:
            version = (int(match.group("major")), int(match.group("minor")))
            arch = match.group("arch")
        else:
            aliases = [
                (name, value)
                for name, value in MANYLINUX_ALIASES.items()
                if platform.startswith(name + "_")
            ]
            if len(aliases) != 1:
                incompatible.append(tag)
                continue
            alias, version = aliases[0]
            arch = platform[len(alias) + 1 :]
        if arch != required_arch or version > required_version:
            incompatible.append(tag)
    if incompatible:
        raise WheelCheckError(
            f"wheel tags exceed required policy {required}: {sorted(incompatible)}"
        )


def digest(content: bytes) -> str:
    encoded = base64.urlsafe_b64encode(hashlib.sha256(content).digest()).rstrip(b"=")
    return "sha256=" + encoded.decode("ascii")


def validate_record(
    archive: zipfile.ZipFile, members: dict[str, zipfile.ZipInfo]
) -> str:
    records = [name for name in members if name.endswith(".dist-info/RECORD")]
    if len(records) != 1:
        raise WheelCheckError(f"expected one RECORD file, found {len(records)}")
    record_name = records[0]
    rows = list(csv.reader(archive.read(record_name).decode("utf-8").splitlines()))
    recorded: set[str] = set()
    for row in rows:
        if len(row) != 3:
            raise WheelCheckError("wheel RECORD contains a malformed row")
        name, expected_digest, expected_size = row
        if name in recorded or name not in members:
            raise WheelCheckError(f"wheel RECORD has an unknown or duplicate path: {name}")
        recorded.add(name)
        if name == record_name:
            if expected_digest or expected_size:
                raise WheelCheckError("wheel RECORD must leave its own digest empty")
            continue
        content = archive.read(name)
        if expected_digest != digest(content) or expected_size != str(len(content)):
            raise WheelCheckError(f"wheel RECORD digest mismatch: {name}")
    if recorded != set(members):
        missing = sorted(set(members) - recorded)
        raise WheelCheckError(f"wheel RECORD omits {len(missing)} member(s): {missing[:3]}")
    return record_name


def extract(archive: zipfile.ZipFile, root: Path) -> None:
    if root.exists():
        shutil.rmtree(root)
    root.mkdir(parents=True)
    for info in archive.infolist():
        logical = safe_name(info.filename)
        target = root.joinpath(*logical.parts)
        if info.is_dir():
            target.mkdir(parents=True, exist_ok=True)
            continue
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(archive.read(info))
        mode = (info.external_attr >> 16) & 0o777
        target.chmod(mode or 0o644)


def reject_forbidden_prefixes(
    archive: zipfile.ZipFile,
    members: dict[str, zipfile.ZipInfo],
    prefixes: Sequence[str],
) -> None:
    needles = [os.fsencode(str(Path(prefix).resolve())) for prefix in prefixes]
    offenders: list[str] = []
    for name, info in members.items():
        if info.is_dir():
            continue
        content = archive.read(name)
        if any(needle in content for needle in needles):
            offenders.append(name)
    if offenders:
        raise WheelCheckError(
            "wheel contains an absolute build-source prefix in: "
            + ", ".join(sorted(offenders)[:8])
        )


def dynamic_section(readelf: str, path: Path) -> dict[str, Any]:
    result = subprocess.run(
        [readelf, "-d", str(path)],
        check=False,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    if result.returncode:
        raise WheelCheckError(
            f"readelf failed for {path}: {result.stderr.strip() or result.stdout.strip()}"
        )
    needed = sorted(match.group("value") for match in NEEDED.finditer(result.stdout))
    paths = [match.group("value") for match in RUNPATH.finditer(result.stdout)]
    sonames = [match.group("value") for match in SONAME.finditer(result.stdout)]
    return {"needed": needed, "runtime_paths": paths, "sonames": sonames}


def defined_dynamic_symbols(readelf: str, path: Path) -> list[str]:
    result = subprocess.run(
        [readelf, "--dyn-syms", "--wide", str(path)],
        check=False,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    if result.returncode:
        raise WheelCheckError(
            f"readelf failed for {path}: {result.stderr.strip() or result.stdout.strip()}"
        )
    symbols: set[str] = set()
    for line in result.stdout.splitlines():
        fields = line.split()
        if (
            len(fields) >= 8
            and fields[0].endswith(":")
            and fields[4] in {"GLOBAL", "WEAK"}
            and fields[6] != "UND"
        ):
            symbols.add(fields[7].split("@", 1)[0])
    return sorted(symbols)


def require_safe_runtime_paths(paths: Sequence[str], binary: Path, site: Path) -> None:
    site = site.resolve()
    for group in paths:
        for value in group.split(":"):
            if value == "$ORIGIN":
                target = binary.parent
            elif value.startswith("$ORIGIN/"):
                target = binary.parent / value[len("$ORIGIN/") :]
            else:
                raise WheelCheckError(
                    f"{binary.name} has a non-wheel runtime path: {value!r}"
                )
            try:
                target.resolve().relative_to(site)
            except ValueError as error:
                raise WheelCheckError(
                    f"{binary.name} runtime path escapes the wheel: {value!r}"
                ) from error


def require_dependency_closure(
    dynamic: dict[str, Any],
    allowed_platform: set[str],
    bundled_names: set[str],
    label: str,
) -> None:
    missing = sorted(
        set(dynamic["needed"]) - allowed_platform - bundled_names
    )
    if missing:
        raise WheelCheckError(
            f"{label} has unbundled dependencies: {', '.join(missing)}"
        )


def run(
    arguments: Sequence[str], cwd: Path, environment: dict[str, str], output: Path
) -> subprocess.CompletedProcess[str]:
    result = subprocess.run(
        list(arguments),
        cwd=cwd,
        env=environment,
        check=False,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    output.parent.mkdir(parents=True, exist_ok=True)
    output.with_suffix(".stdout").write_text(result.stdout, encoding="utf-8")
    output.with_suffix(".stderr").write_text(result.stderr, encoding="utf-8")
    if result.returncode:
        raise WheelCheckError(
            f"{' '.join(arguments)} failed with exit status {result.returncode}; "
            f"see {output.with_suffix('.stderr')}"
        )
    return result


def parse_scenario_marker(stdout: str) -> dict[str, list[str]]:
    lines = [
        line[len(SCENARIO_MARKER) + 1 :]
        for line in stdout.splitlines()
        if line.startswith(SCENARIO_MARKER + " ")
    ]
    if len(lines) != 1:
        raise WheelCheckError(
            f"expected one {SCENARIO_MARKER} marker, found {len(lines)}"
        )
    try:
        document = json.loads(lines[0])
    except json.JSONDecodeError as error:
        raise WheelCheckError(f"invalid {SCENARIO_MARKER} JSON: {error}") from error
    if not isinstance(document, dict) or document.get("schema_version") != 1:
        raise WheelCheckError(f"invalid {SCENARIO_MARKER} document")
    passed = document.get("passed")
    skipped = document.get("skipped")
    if not isinstance(passed, list) or not all(isinstance(item, str) for item in passed):
        raise WheelCheckError("Python scenario marker has an invalid passed list")
    if skipped != []:
        raise WheelCheckError(f"required Python scenarios were skipped: {skipped}")
    if set(passed) != set(EXPECTED_SCENARIOS) or len(passed) != len(
        EXPECTED_SCENARIOS
    ):
        missing = sorted(set(EXPECTED_SCENARIOS) - set(passed))
        extra = sorted(set(passed) - set(EXPECTED_SCENARIOS))
        raise WheelCheckError(
            f"Python scenario execution is incomplete: missing={missing}, extra={extra}"
        )
    measured: dict[str, list[str]] = {}
    for scenario in passed:
        measured.setdefault(EXPECTED_SCENARIOS[scenario], []).append(scenario)
    return {name: sorted(scenarios) for name, scenarios in sorted(measured.items())}


def sanitized_environment() -> dict[str, str]:
    environment = dict(os.environ)
    environment["PYTHONDONTWRITEBYTECODE"] = "1"
    environment["PYTHONNOUSERSITE"] = "1"
    for name in (
        "LD_LIBRARY_PATH",
        "LD_PRELOAD",
        "POSTGAMMA_LIBRARY",
        "POSTGAMMA_STATIC_LIBRARY",
        "POSTGAMMA_STATIC_LINK_OPTIONS",
        "POSTGAMMA_STATIC_RECEIPT",
        "POSTGAMMA_RESOURCE_ROOT",
        "PYTHONPATH",
    ):
        environment.pop(name, None)
    return environment


def prepare_work_root(root: Path) -> Path:
    """Remove every checker-owned directory before inspecting or running a wheel."""

    resolved = root.resolve()
    for name in (
        "audit-site",
        "installed-site",
        "execution",
        "examples",
        "integration",
        "pip-install",
        "site",  # Clean stale output from the original Python checker.
        "unit",
    ):
        path = resolved / name
        if path.is_dir():
            shutil.rmtree(path)
        elif path.exists():
            path.unlink()
    resolved.mkdir(parents=True, exist_ok=True)
    return resolved


def execute_documentation_examples(
    *,
    python: str,
    project_root: Path,
    work_root: Path,
    environment: dict[str, str],
    expected_names: Sequence[str],
) -> dict[str, Any]:
    examples = python_example_manifest(project_root)
    names = [entry["name"] for entry in examples]
    if names != list(expected_names):
        raise WheelCheckError(
            "Python documentation examples differ from the release baseline: "
            f"expected={list(expected_names)}, actual={names}"
        )
    output_hashes: dict[str, str] = {}
    for entry in examples:
        example_work = work_root / "examples" / entry["name"]
        example_work.mkdir(parents=True, exist_ok=False)
        result = run(
            (
                python,
                str(project_root / "examples/python" / entry["path"]),
            ),
            example_work,
            environment,
            work_root / "examples" / (entry["name"] + "-run"),
        )
        if result.stdout != entry["stdout"]:
            raise WheelCheckError(
                f"Python documentation example {entry['name']} produced "
                f"unexpected stdout: expected={entry['stdout']!r}, "
                f"actual={result.stdout!r}"
            )
        output_hashes[entry["name"]] = hashlib.sha256(
            result.stdout.encode("utf-8")
        ).hexdigest()
    return {
        "count": len(names),
        "names": names,
        "stdout_sha256": output_hashes,
    }


def install_wheel(
    args: argparse.Namespace,
    wheel: Path,
    audit_site: Path,
    environment: dict[str, str],
) -> tuple[Path, subprocess.CompletedProcess[str] | None]:
    if args.installer == "archive":
        return audit_site, None
    install_site = args.work_root.resolve() / "installed-site"
    if install_site.exists():
        shutil.rmtree(install_site)
    install_site.parent.mkdir(parents=True, exist_ok=True)
    result = run(
        (
            args.python,
            "-m",
            "pip",
            "--isolated",
            "--disable-pip-version-check",
            "install",
            "--no-index",
            "--no-deps",
            "--no-compile",
            "--target",
            str(install_site),
            str(wheel),
        ),
        args.work_root.resolve(),
        environment,
        args.work_root.resolve() / "pip-install",
    )
    if not (install_site / "postgamma/__init__.py").is_file():
        raise WheelCheckError("pip did not install the PostGamma package")
    return install_site, result


def check(args: argparse.Namespace) -> dict[str, Any]:
    work_root = prepare_work_root(args.work_root)
    baseline_path = args.baseline.resolve()
    baseline = load_release_baseline(baseline_path)
    receipt = load_json(args.receipt.resolve())
    if receipt.get("schema_version") != 2 or receipt.get("kind") != (
        "postgamma.python-wheel-build"
    ):
        raise WheelCheckError("invalid Python wheel build receipt")
    if (
        receipt.get("name") != baseline["release"]["name"]
        or receipt.get("version") != baseline["release"]["version"]
        or receipt.get("metadata_version") != baseline["wheel"]["metadata_version"]
        or receipt.get("requires_python") != baseline["python"]["requires_python"]
        or receipt.get("project_url")
        != baseline["wheel"]["project_urls"]["Homepage"]
        or receipt.get("kernel_linkage") != "static"
        or receipt.get("bundled_kernel_library") is not False
        or not isinstance(receipt.get("kernel_archive_sha256"), str)
        or re.fullmatch(r"[0-9a-f]{64}", receipt["kernel_archive_sha256"])
        is None
        or receipt.get("license_files")
        != sorted(release_license_files(baseline))
    ):
        raise WheelCheckError("Python wheel receipt has an invalid identity")
    wheel = args.wheel_dir.resolve() / str(receipt.get("filename", ""))
    if not wheel.is_file():
        raise WheelCheckError(f"wheel from receipt is missing: {wheel}")
    wheel_files = sorted(args.wheel_dir.resolve().glob("*.whl"))
    if wheel_files != [wheel]:
        raise WheelCheckError(
            "wheel directory must contain exactly the artifact in its receipt"
        )
    content = wheel.read_bytes()
    if receipt.get("sha256") != hashlib.sha256(content).hexdigest() or receipt.get(
        "size"
    ) != len(content):
        raise WheelCheckError("Python wheel no longer matches its build receipt")
    with zipfile.ZipFile(wheel) as archive:
        members = {info.filename: info for info in archive.infolist()}
        if len(members) != len(archive.infolist()):
            raise WheelCheckError("wheel contains duplicate member names")
        for name in members:
            safe_name(name)
        record = validate_record(archive, members)
        tags, metadata = validate_metadata(
            archive, members, record, receipt, wheel.name, baseline
        )
        if args.require_platform is not None:
            require_platform_policy(tags, args.require_platform)
            if receipt.get("platform_policy") != args.require_platform:
                raise WheelCheckError("wheel receipt does not bind the platform policy")
        reject_forbidden_prefixes(archive, members, args.forbidden_prefix)
        native_members = [
            name
            for name in members
            if name.startswith("postgamma/_native") and name.endswith(".so")
        ]
        required = {
            "postgamma/__init__.py",
            "postgamma/py.typed",
            "postgamma/_resources/bin/postgres",
            STATIC_KERNEL_MARKER,
            "postgamma/_resources/share/postgres.bki",
        }
        dist_info = record.rsplit("/", 1)[0]
        required.update(
            f"{dist_info}/licenses/{name}"
            for name in release_license_files(baseline)
        )
        bundled_kernels = sorted(
            name
            for name in members
            if PurePosixPath(name).name.startswith("libpostgamma")
            and ".so" in PurePosixPath(name).name
        )
        if bundled_kernels:
            raise WheelCheckError(
                "statically linked wheel contains a PostGamma shared library: "
                + ", ".join(bundled_kernels)
            )
        try:
            static_marker = json.loads(archive.read(STATIC_KERNEL_MARKER))
        except (KeyError, json.JSONDecodeError, UnicodeDecodeError) as error:
            raise WheelCheckError("wheel static-kernel marker is invalid") from error
        if static_marker != {
            "schema_version": 1,
            "kernel_linkage": "static",
            "separate_library": False,
        }:
            raise WheelCheckError("wheel static-kernel marker changed")
        missing = sorted(required - set(members))
        if missing or len(native_members) != 1:
            raise WheelCheckError(
                f"wheel runtime closure is incomplete: missing={missing}, "
                f"native_extension_count={len(native_members)}"
            )
        postgres_mode = (members["postgamma/_resources/bin/postgres"].external_attr >> 16)
        if not postgres_mode & stat.S_IXUSR:
            raise WheelCheckError("bundled PostgreSQL executable identity is not executable")
        extract(archive, work_root / "audit-site")
    audit_site = work_root / "audit-site"
    native = audit_site / native_members[0]
    postgres = audit_site / "postgamma/_resources/bin/postgres"
    native_dynamic = dynamic_section(args.readelf, native)
    native_exports = defined_dynamic_symbols(args.readelf, native)
    postgres_dynamic = dynamic_section(args.readelf, postgres)
    bundled_names = {
        path.name for path in audit_site.rglob("*") if path.is_file() and ".so" in path.name
    }
    if args.require_platform is None:
        if native_dynamic["runtime_paths"]:
            raise WheelCheckError(
                f"statically linked Python extension has runtime paths: "
                f"{native_dynamic['runtime_paths']}"
            )
    else:
        require_safe_runtime_paths(native_dynamic["runtime_paths"], native, audit_site)
    postgamma_dependencies = sorted(
        name for name in native_dynamic["needed"] if name.startswith("libpostgamma")
    )
    if postgamma_dependencies:
        raise WheelCheckError(
            "statically linked Python extension depends on a PostGamma shared library: "
            + ", ".join(postgamma_dependencies)
        )
    if native_exports != [NATIVE_DYNAMIC_EXPORT]:
        raise WheelCheckError(
            f"Python extension exports {native_exports}, expected "
            f"[{NATIVE_DYNAMIC_EXPORT!r}]"
        )
    if args.require_platform is None:
        unexpected_native = sorted(
            set(native_dynamic["needed"]) - ALLOWED_NATIVE_DEPENDENCIES
        )
        if unexpected_native:
            raise WheelCheckError(
                "Python extension has unexpected dependencies: "
                + ", ".join(unexpected_native)
            )
        if postgres_dynamic["runtime_paths"]:
            raise WheelCheckError(
                f"PostgreSQL executable has captured runtime paths: "
                f"{postgres_dynamic['runtime_paths']}"
            )
        require_dependency_closure(
            postgres_dynamic,
            ALLOWED_KERNEL_DEPENDENCIES,
            set(),
            "PostgreSQL executable",
        )
    else:
        require_dependency_closure(
            native_dynamic,
            MANYLINUX_PLATFORM_DEPENDENCIES,
            bundled_names,
            "Python extension",
        )
        require_safe_runtime_paths(
            postgres_dynamic["runtime_paths"], postgres, audit_site
        )
        require_dependency_closure(
            postgres_dynamic,
            MANYLINUX_PLATFORM_DEPENDENCIES,
            bundled_names,
            "PostgreSQL executable",
        )
    environment = sanitized_environment()
    site, pip_result = install_wheel(args, wheel, audit_site, environment)
    environment["PYTHONPATH"] = str(site)
    execution_root = work_root / "execution"
    execution_root.mkdir(parents=True, exist_ok=True)
    public_api = probe_public_api(
        args.python,
        site,
        environment,
        args.work_root.resolve() / "public-api",
        baseline,
    )
    unit_result = run(
        (
            args.python,
            "-m",
            "unittest",
            "discover",
            "-s",
            str(args.tests.resolve()),
            "-p",
            "test_*.py",
            "-v",
        ),
        execution_root,
        environment,
        args.work_root.resolve() / "unit",
    )
    integration_result = run(
        (args.python, str(args.integration.resolve()), "--verbosity", "2"),
        execution_root,
        environment,
        args.work_root.resolve() / "integration",
    )
    scenario_evidence = parse_scenario_marker(integration_result.stdout)
    documentation_examples = execute_documentation_examples(
        python=args.python,
        project_root=args.project_root.resolve(),
        work_root=work_root,
        environment=environment,
        expected_names=baseline["documentation"]["examples"],
    )
    gate_evidence: dict[str, Any] = {
        "unit": {"returncode": unit_result.returncode},
        "integration": {
            "returncode": integration_result.returncode,
            "scenario_count": len(EXPECTED_SCENARIOS),
        },
        "documentation_examples": documentation_examples,
        "installation": {
            "mode": args.installer,
            "pip_returncode": pip_result.returncode if pip_result is not None else None,
        },
        "public_api": {
            "export_count": len(public_api["exports"]),
            "baseline_sha256": hashlib.sha256(baseline_path.read_bytes()).hexdigest(),
        },
        "runtime_closure": {"required_members": sorted(required)},
        "no_build_source_paths": {"forbidden_prefix_count": len(args.forbidden_prefix)},
    }
    gate_evidence.update(
        {name: {"scenarios": scenarios} for name, scenarios in scenario_evidence.items()}
    )
    return {
        "schema_version": 2,
        "kind": REPORT_KIND,
        "status": "pass",
        "wheel": {
            "filename": wheel.name,
            "sha256": hashlib.sha256(content).hexdigest(),
            "size": len(content),
            "member_count": len(members),
            "record": record,
            "tags": tags,
            "platform_policy": receipt.get("platform_policy"),
        },
        "metadata": metadata,
        "release_baseline": {
            "sha256": hashlib.sha256(baseline_path.read_bytes()).hexdigest(),
            "public_api_version": baseline["release"]["public_api_version"],
        },
        "native_extension": native_dynamic,
        "kernel": {
            "linkage": "static",
            "separate_library": False,
            "postgamma_dynamic_dependencies": len(postgamma_dependencies),
            "native_dynamic_exports": native_exports,
        },
        "postgres_executable": postgres_dynamic,
        "resource_root": {"members": sorted(required)},
        "gates": gate_evidence,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--wheel-dir", required=True, type=Path)
    parser.add_argument("--receipt", required=True, type=Path)
    parser.add_argument("--baseline", required=True, type=Path)
    parser.add_argument("--tests", required=True, type=Path)
    parser.add_argument("--integration", required=True, type=Path)
    parser.add_argument("--project-root", required=True, type=Path)
    parser.add_argument("--work-root", required=True, type=Path)
    parser.add_argument("--forbidden-prefix", action="append", default=[])
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--python", default=sys.executable)
    parser.add_argument("--readelf", default="readelf")
    parser.add_argument("--installer", choices=("archive", "pip"), default="archive")
    parser.add_argument("--require-platform")
    args = parser.parse_args()
    try:
        report = check(args)
    except (OSError, WheelCheckError, zipfile.BadZipFile) as error:
        parser.error(str(error))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(
        json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    print(
        f"Python wheel: {report['wheel']['filename']} passed unit, integration, "
        "streaming, COPY, async, management, Arrow, and resource gates"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
