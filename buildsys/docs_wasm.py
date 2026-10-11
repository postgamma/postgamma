#!/usr/bin/env python3
"""Stage a complete production Wasm package for the documentation live demo."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import shutil
import tarfile
import tempfile


def verify(package: Path) -> dict:
    metadata = json.loads((package / "package.json").read_text())
    manifest = json.loads((package / "postgamma.assets.json").read_text())
    if (metadata.get("name") != "@postgamma/wasm" or
            metadata.get("version") != manifest.get("version") or
            manifest.get("bridge", {}).get("testing") is not False):
        raise ValueError("live demo requires a matching production @postgamma/wasm package")
    for path in package.rglob("*"):
        if path.is_symlink():
            raise ValueError("browser package must not contain symlinks")
    for name, expected in manifest["files"].items():
        path = package / name
        if not path.resolve().is_relative_to(package.resolve()) or not path.is_file():
            raise ValueError("invalid browser package asset: " + name)
        if path.stat().st_size != expected["bytes"] or hashlib.sha256(path.read_bytes()).hexdigest() != expected["sha256"]:
            raise ValueError("browser package integrity mismatch: " + name)
    for name in (*manifest["entries"].values(), "src/index.js", "src/loader/identity.js", "LICENSE", "NOTICE"):
        if not (package / name).is_file() or not (package / name).resolve().is_relative_to(package.resolve()):
            raise ValueError("browser package is incomplete: " + name)
    return {"available": True, "version": metadata["version"], "buildId": manifest["buildId"],
            "bytes": sum(p.stat().st_size for p in package.rglob("*") if p.is_file() and p.name != "demo.json")}


def unpack(archive: Path, destination: Path) -> None:
    with tarfile.open(archive) as source:
        members = source.getmembers()
        if len(members) > 10000 or sum(m.size for m in members) > 256 * 1024 * 1024:
            raise ValueError("browser package exceeds the documentation asset budget")
        for member in members:
            path = PurePosixPath(member.name)
            if not path.parts or path.parts[0] != "package" or ".." in path.parts or path.is_absolute():
                raise ValueError("invalid package archive path")
            target = destination.joinpath(*path.parts[1:])
            if member.isdir():
                target.mkdir(parents=True, exist_ok=True)
            elif member.isfile():
                target.parent.mkdir(parents=True, exist_ok=True)
                with source.extractfile(member) as reader, target.open("wb") as writer:
                    shutil.copyfileobj(reader, writer)
            else:
                raise ValueError("package archive must contain only regular files and directories")


def latest_package(build: Path) -> Path:
    pointers = sorted((build / "wasm/wasm32-emscripten-pthreads/release").glob("*/variants/*/reports/package.json"),
                      key=lambda p: p.stat().st_mtime, reverse=True)
    for pointer in pointers:
        summary = json.loads(pointer.read_text())
        if summary.get("status") != "passed":
            continue
        report = json.loads(Path(summary["run_report"]).read_text())
        if report.get("status") == "passed":
            return Path(report["package"])
    raise ValueError("no successful release package found; run make wasm-package WASM_MODE=release first")


def stage(build: Path, source: str = "") -> dict:
    root = build / "docs/browser"
    target = root / "assets/wasm"
    target.parent.mkdir(parents=True, exist_ok=True)
    if source:
        package = latest_package(build) if source == "auto" else Path(source).resolve()
        with tempfile.TemporaryDirectory(prefix=".wasm-stage-", dir=root) as temporary:
            prepared = Path(temporary) / "package"
            if package.is_dir():
                verify(package)
                shutil.copytree(package, prepared)
            else:
                prepared.mkdir()
                unpack(package, prepared)
            info = verify(prepared)
            # Only this tool's generated directory is replaced. A failed verify
            # leaves the last usable package untouched.
            if target.exists():
                shutil.rmtree(target)
            prepared.rename(target)
    elif (target / "postgamma.assets.json").is_file():
        info = verify(target)
    else:
        info = {"available": False}
        target.mkdir(exist_ok=True)
    (target / "demo.json").write_text(json.dumps(info, indent=2) + "\n")
    return info


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=Path("build"))
    parser.add_argument("--package", default="")
    args = parser.parse_args()
    try:
        info = stage(args.build_dir.resolve(), args.package)
    except (OSError, ValueError, KeyError, tarfile.TarError) as error:
        parser.exit(2, "documentation browser package: " + str(error) + "\n")
    print("documentation browser demo: " + (info["version"] + " (" + info["buildId"] + ")" if info["available"]
          else "not bundled; set DOCS_WASM_PACKAGE=/path/to/postgamma-wasm.tgz to enable"))


if __name__ == "__main__":
    main()
