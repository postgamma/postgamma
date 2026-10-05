#!/usr/bin/env python3
"""Link the bootstrap static-extension probe with explicit ELF safety gates."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import subprocess
import tempfile
from pathlib import Path


RECEIPT_KIND = "postgamma.bootstrap-static-extension-link"
REQUIRED_FLAGS = ("-shared", "-Wl,-z,defs", "-Wl,-Bsymbolic-functions")


class StaticExtensionLinkError(RuntimeError):
    """The bootstrap shared-library link failed or used stale inputs."""


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def link_command(
    compiler: str,
    objects: list[Path],
    archive: Path,
    response_file: Path,
    version_script: Path,
    output: Path,
) -> list[str]:
    return [
        compiler,
        *REQUIRED_FLAGS,
        f"-Wl,--version-script={version_script}",
        *(str(path) for path in objects),
        str(archive),
        f"@{response_file}",
        "-pthread",
        "-o",
        str(output),
    ]


def write_json(path: Path, document: dict[str, object]) -> None:
    content = json.dumps(document, indent=2, sort_keys=True) + "\n"
    path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(
        mode="w", encoding="utf-8", dir=path.parent, prefix=f".{path.name}.", delete=False
    ) as handle:
        handle.write(content)
        temporary = Path(handle.name)
    try:
        os.replace(temporary, path)
    finally:
        temporary.unlink(missing_ok=True)


def link(
    compiler: str,
    work_directory: Path,
    objects: list[Path],
    archive: Path,
    response_file: Path,
    version_script: Path,
    output: Path,
    receipt: Path,
) -> dict[str, object]:
    inputs = [*objects, archive, response_file, version_script]
    missing = [path for path in inputs if not path.is_file()]
    if missing:
        raise StaticExtensionLinkError(
            "link input(s) missing: " + ", ".join(str(path) for path in missing)
        )
    if not work_directory.is_dir():
        raise StaticExtensionLinkError(
            f"link working directory does not exist: {work_directory}"
        )
    output.parent.mkdir(parents=True, exist_ok=True)
    output.unlink(missing_ok=True)
    receipt.unlink(missing_ok=True)
    command = link_command(
        compiler, objects, archive, response_file, version_script, output
    )
    try:
        completed = subprocess.run(
            command,
            cwd=work_directory,
            check=False,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
    except OSError as exc:
        raise StaticExtensionLinkError(f"cannot execute linker: {exc}") from exc
    if completed.returncode != 0:
        diagnostic = completed.stderr.strip() or completed.stdout.strip()
        raise StaticExtensionLinkError(
            f"shared-library link failed with status {completed.returncode}: {diagnostic}"
        )
    if not output.is_file():
        raise StaticExtensionLinkError("linker reported success without an output file")
    document: dict[str, object] = {
        "schema_version": 1,
        "kind": RECEIPT_KIND,
        "command": command,
        "work_directory": str(work_directory),
        "required_flags": list(REQUIRED_FLAGS),
        "inputs": {str(path): sha256(path) for path in inputs},
        "output": str(output),
        "output_sha256": sha256(output),
    }
    write_json(receipt, document)
    return document


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", default="cc")
    parser.add_argument("--work-directory", required=True, type=Path)
    parser.add_argument("--object", action="append", default=[], type=Path)
    parser.add_argument("--archive", required=True, type=Path)
    parser.add_argument("--response-file", required=True, type=Path)
    parser.add_argument("--version-script", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--receipt", required=True, type=Path)
    args = parser.parse_args()
    if not args.object:
        parser.error("at least one --object is required")
    try:
        document = link(
            args.compiler,
            args.work_directory.resolve(),
            [path.resolve() for path in args.object],
            args.archive.resolve(),
            args.response_file.resolve(),
            args.version_script.resolve(),
            args.output.resolve(),
            args.receipt.resolve(),
        )
    except StaticExtensionLinkError as exc:
        parser.error(str(exc))
    print(
        f"embedded static-extension link: {document['output']} "
        "(undefined symbols rejected)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
