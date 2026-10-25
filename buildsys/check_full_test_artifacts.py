#!/usr/bin/env python3
"""Export or test the same SDK and wheel on another host, without rebuilding them."""

from __future__ import annotations

import argparse
import json
import platform
import shlex
import shutil
import subprocess
import sys
import tarfile
import tempfile
from pathlib import Path, PurePosixPath

from check_installed_c_sdk import marker_fields, require_success
from check_python_wheel import SCENARIO_MARKER, check as check_wheel
from check_runtime_libraries import (
    RuntimeLibraryError, clean_environment, inspect_libraries, verify_loaded_libraries,
)
from check_static_sdk import pgvector_marker_fields, run_quickstart, selects_static_archive, sha256


KIND = "postgamma.full-test-artifacts"


def safe_path(value: str) -> Path:
    path = PurePosixPath(value)
    if not path.parts or path.is_absolute() or ".." in path.parts or str(path) != value:
        raise RuntimeLibraryError(f"invalid artifact member: {value!r}")
    return Path(*path.parts)


def export_artifacts(root: Path, build: Path, destination: Path) -> None:
    sdk = list((build / "dist").glob("postgamma-sdk-*.tar.gz"))
    if len(sdk) != 1:
        raise RuntimeLibraryError("expected exactly one packaged static SDK")
    receipt_path = build / "python-product/reports/wheel-build.json"
    receipt = json.loads(receipt_path.read_text())
    wheel = build / "python-product/wheel" / safe_path(receipt["filename"])
    if sha256(wheel) != receipt["sha256"]:
        raise RuntimeLibraryError("wheel differs from its build receipt")
    files = {
        "sdk/" + sdk[0].name: sdk[0],
        "wheel/" + wheel.name: wheel,
        "wheel/receipt.json": receipt_path,
    }
    if destination.is_symlink():
        raise RuntimeLibraryError(f"artifact directory is a symlink: {destination}")
    if destination.exists() and any(destination.iterdir()):
        marker = destination / "manifest.json"
        if not marker.is_file() or json.loads(marker.read_text()).get("kind") != KIND:
            raise RuntimeLibraryError(f"refusing to replace an unmarked artifact directory: {destination}")
        for child in (destination / "sdk", destination / "wheel"):
            if child.is_symlink():
                raise RuntimeLibraryError(f"artifact directory is a symlink: {child}")
            if child.exists():
                shutil.rmtree(child)
        marker.unlink()
    destination.mkdir(parents=True, exist_ok=True)
    for name, source in files.items():
        target = destination / name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, target)
    document = {
        "schema_version": 1,
        "kind": KIND,
        "source_commit": subprocess.check_output(
            ["git", "-C", str(root), "rev-parse", "HEAD"], text=True,
        ).strip(),
        "build_host": platform.platform(),
        "build_profile": "system-libraries",
        "sdk": "sdk/" + sdk[0].name,
        "wheel_receipt": "wheel/receipt.json",
        "files": {name: sha256(source) for name, source in sorted(files.items())},
    }
    (destination / "manifest.json").write_text(json.dumps(document, indent=2) + "\n")


def unpack_sdk(archive_path: Path, directory: Path) -> Path:
    with tarfile.open(archive_path) as archive:
        seen: set[str] = set()
        for member in archive:
            name = member.name.rstrip("/") if member.isdir() else member.name
            relative = safe_path(name)
            if name in seen:
                raise RuntimeLibraryError(f"duplicate SDK member: {name}")
            seen.add(name)
            target = directory / relative
            if member.isdir():
                target.mkdir(parents=True, exist_ok=True)
            elif member.isfile():
                target.parent.mkdir(parents=True, exist_ok=True)
                with archive.extractfile(member) as source, target.open("wb") as output:
                    shutil.copyfileobj(source, output)
                target.chmod(member.mode & 0o777)
            else:
                raise RuntimeLibraryError(f"unsupported SDK member: {name}")
    roots = list(directory.iterdir())
    if len(roots) != 1 or not roots[0].is_dir():
        raise RuntimeLibraryError("SDK archive must have exactly one root directory")
    sdk = roots[0]
    manifest = json.loads((sdk / "MANIFEST.json").read_text())
    if manifest.get("kind") != "postgamma.static-sdk-manifest" or manifest.get("schema_version") != 1:
        raise RuntimeLibraryError("invalid SDK manifest")
    expected: set[Path] = set()
    for entry in manifest["files"]:
        path = sdk / safe_path(entry["path"])
        if path in expected or not path.is_file() or sha256(path) != entry["sha256"]:
            raise RuntimeLibraryError(f"SDK manifest mismatch: {entry['path']}")
        expected.add(path)
    actual = {path for path in sdk.rglob("*") if path.is_file()} - {sdk / "MANIFEST.json"}
    if expected != actual:
        raise RuntimeLibraryError("SDK manifest does not describe every payload file")
    return sdk


def check(args: argparse.Namespace) -> dict[str, object]:
    root = args.root.resolve(strict=True)
    artifacts = args.artifacts.resolve(strict=True)
    manifest = json.loads((artifacts / "manifest.json").read_text())
    if manifest.get("schema_version") != 1 or manifest.get("kind") != KIND:
        raise RuntimeLibraryError("invalid full-test artifact manifest")
    commit = subprocess.check_output(["git", "-C", str(root), "rev-parse", "HEAD"], text=True).strip()
    if manifest.get("source_commit") != commit:
        raise RuntimeLibraryError("use the source commit recorded in the artifact manifest")
    for name, digest in manifest["files"].items():
        path = artifacts / safe_path(name)
        if path.is_symlink() or sha256(path) != digest:
            raise RuntimeLibraryError(f"artifact checksum mismatch: {name}")
    for name in (manifest["sdk"], manifest["wheel_receipt"]):
        if name not in manifest["files"]:
            raise RuntimeLibraryError(f"artifact identity missing from manifest: {name}")
    work = args.work_root.resolve()
    work.mkdir(parents=True, exist_ok=True)
    environment = clean_environment()
    dependencies: dict[str, object] = {}
    maximum = tuple(map(int, args.maximum_glibc.split("."))) if args.maximum_glibc else None

    with tempfile.TemporaryDirectory(prefix="artifact-sdk-", dir=work) as temporary:
        temporary_root = Path(temporary)
        sdk = unpack_sdk(artifacts / manifest["sdk"], temporary_root / "unpacked")
        resources = sdk / "resource-pack"
        # pkg-config needs only this relocated SDK; compilation and execution
        # must not inherit the build toolchain's library search paths.
        flags = shlex.split(subprocess.check_output(
            [args.pkg_config, "--cflags", "--static", "--libs", "postgamma"],
            env={**environment, "PKG_CONFIG_PATH": str(sdk / "lib/pkgconfig")},
            text=True, timeout=30,
        ))
        if not selects_static_archive(flags, sdk / "lib/libpostgamma.a") or "-lpostgamma" in flags:
            raise RuntimeLibraryError("SDK pkg-config does not select its static archive")
        for label, source in (
            ("quickstart", sdk / "examples/quickstart.c"),
            ("pgvector", root / "tests/embedded/release/static_pgvector_driver.c"),
        ):
            executable = temporary_root / label
            subprocess.run(
                [args.cc, "-std=c11", "-Wall", "-Wextra", "-Werror", str(source),
                 *flags, "-o", str(executable)],
                env=environment, check=True, timeout=120,
            )
            dependencies[label] = inspect_libraries(executable, readelf=args.readelf, forbidden=args.forbidden_prefix)
            if label == "quickstart":
                result = run_quickstart(executable=executable, resource_root=resources,
                                        data_directory=temporary_root / "data", environment=environment)
                require_success("packaged SDK quickstart", result)
                marker_fields(result.stdout)
            else:
                result = subprocess.run(
                    [str(executable), str(temporary_root / "vector-data"),
                     str(resources / "bin/postgres"), str(resources)],
                    env=environment, capture_output=True, text=True, timeout=300,
                )
                require_success("packaged SDK pgvector", result)
                pgvector_marker_fields(result.stdout)
        for path in sorted(resources.rglob("*")):
            if path.is_file():
                with path.open("rb") as stream:
                    is_elf = stream.read(4) == b"\x7fELF"
                if is_elf:
                    dependencies["sdk/" + str(path.relative_to(resources))] = inspect_libraries(
                        path, readelf=args.readelf, forbidden=args.forbidden_prefix,
                        maximum_glibc=maximum,
                    )

    receipt_path = artifacts / safe_path(manifest["wheel_receipt"])
    receipt = json.loads(receipt_path.read_text())
    wheel_path = receipt_path.parent / safe_path(receipt["filename"])
    if str(wheel_path.relative_to(artifacts)) not in manifest["files"]:
        raise RuntimeLibraryError("wheel identity missing from artifact manifest")
    policy = receipt["platform_policy"]
    # None selects native dependency and runtime-path checks; an explicit
    # platform requirement selects the manylinux checks.
    wheel_report = check_wheel(argparse.Namespace(
        wheel_dir=receipt_path.parent, receipt=receipt_path,
        baseline=root / "manifests/api/python-api-v1.json", tests=root / "tests/python",
        integration=root / "tests/python/integration_driver.py", project_root=root,
        work_root=work / "wheel", forbidden_prefix=[], python=args.python,
        readelf=args.readelf, installer="pip",
        require_platform=None if policy == "linux_native" else policy,
    ))
    for path in sorted((work / "wheel/audit-site").rglob("*")):
        if path.is_file():
            with path.open("rb") as stream:
                is_elf = stream.read(4) == b"\x7fELF"
            if is_elf:
                dependencies["wheel/" + str(path.relative_to(work / "wheel/audit-site"))] = inspect_libraries(
                    path, readelf=args.readelf, forbidden=args.forbidden_prefix,
                    maximum_glibc=maximum,
                )
    integration_output = (work / "wheel/integration.stdout").read_text()
    markers = [line[len(SCENARIO_MARKER) + 1:] for line in integration_output.splitlines()
               if line.startswith(SCENARIO_MARKER + " ")]
    if len(markers) != 1:
        raise RuntimeLibraryError("expected one Python integration runtime record")
    runtime = json.loads(markers[0])
    native = Path(runtime["native_module"]).resolve(strict=True)
    if not native.is_relative_to((work / "wheel/installed-site").resolve()):
        raise RuntimeLibraryError("integration did not load the pip-installed extension")
    expected = inspect_libraries(native, readelf=args.readelf, forbidden=args.forbidden_prefix)
    loaded = verify_loaded_libraries(runtime["loaded_objects"], expected["resolved"], readelf=args.readelf)
    return {
        "schema_version": 1, "kind": "postgamma.full-test-artifact-evidence", "status": "pass",
        "source_commit": commit, "host": platform.platform(), "libc": platform.libc_ver(),
        "artifact_files": manifest["files"], "maximum_glibc": args.maximum_glibc,
        "dependencies": dependencies, "python_loaded_dependencies": loaded, "wheel": wheel_report,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--artifacts", type=Path, required=True)
    parser.add_argument("--export-build", type=Path)
    parser.add_argument("--work-root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--cc", default="cc")
    parser.add_argument("--pkg-config", default="pkg-config")
    parser.add_argument("--readelf", default="readelf")
    parser.add_argument("--python", default=sys.executable)
    parser.add_argument("--forbidden-prefix", action="append", type=Path, default=[])
    parser.add_argument("--maximum-glibc")
    args = parser.parse_args()
    args.output.unlink(missing_ok=True)
    try:
        if args.export_build:
            export_artifacts(args.root, args.export_build, args.artifacts)
        report = check(args)
        if args.export_build:
            report["source_dependencies"] = {
                name: inspect_libraries(
                    args.export_build / name / "src/backend/postgres",
                    readelf=args.readelf, forbidden=args.forbidden_prefix,
                ) for name in ("reference", "generated-build")
            }
    except (OSError, ValueError, KeyError, RuntimeError, subprocess.SubprocessError, tarfile.TarError) as error:
        parser.error(str(error))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")
    print("full-test artifacts: packaged SDK and installed wheel passed with host runtime libraries")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
