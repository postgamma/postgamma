#!/usr/bin/env python3
"""Repair one native Linux wheel and bind the manylinux result to a receipt."""

from __future__ import annotations

import argparse
import email.parser
import hashlib
import json
import os
import subprocess
import tempfile
import zipfile
from pathlib import Path
from typing import Any, Sequence

from check_python_wheel import WheelCheckError, filename_tags, load_json


RECEIPT_KIND = "postgamma.python-wheel-build"
REPORT_KIND = "postgamma.python-manylinux-repair"


def run(arguments: Sequence[str]) -> subprocess.CompletedProcess[str]:
    result = subprocess.run(
        list(arguments),
        check=False,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    if result.returncode:
        detail = result.stderr.strip() or result.stdout.strip()
        raise WheelCheckError(
            f"{' '.join(arguments)} failed with exit status {result.returncode}: {detail}"
        )
    return result


def wheel_tags(path: Path) -> list[str]:
    try:
        with zipfile.ZipFile(path) as archive:
            wheel_members = [
                name for name in archive.namelist() if name.endswith(".dist-info/WHEEL")
            ]
            if len(wheel_members) != 1:
                raise WheelCheckError(
                    f"expected one WHEEL metadata member, found {len(wheel_members)}"
                )
            metadata = email.parser.BytesParser().parsebytes(
                archive.read(wheel_members[0])
            )
    except (OSError, zipfile.BadZipFile) as error:
        raise WheelCheckError(f"cannot inspect repaired wheel {path}: {error}") from error
    tags = sorted(metadata.get_all("Tag", []))
    if not tags or tags != filename_tags(path.name):
        raise WheelCheckError("repaired wheel filename and WHEEL tags disagree")
    return tags


def validate_source_receipt(receipt: dict[str, Any], wheel_dir: Path) -> Path:
    if (
        receipt.get("schema_version") != 2
        or receipt.get("kind") != RECEIPT_KIND
        or receipt.get("platform_policy") != "linux_native"
        or receipt.get("kernel_linkage") != "static"
        or receipt.get("bundled_kernel_library") is not False
        or not isinstance(receipt.get("kernel_archive_sha256"), str)
        or len(receipt["kernel_archive_sha256"]) != 64
        or any(
            character not in "0123456789abcdef"
            for character in receipt["kernel_archive_sha256"]
        )
    ):
        raise WheelCheckError("invalid native-wheel build receipt")
    wheel = wheel_dir / str(receipt.get("filename", ""))
    if not wheel.is_file():
        raise WheelCheckError(f"native wheel is missing: {wheel}")
    content = wheel.read_bytes()
    if (
        hashlib.sha256(content).hexdigest() != receipt.get("sha256")
        or len(content) != receipt.get("size")
    ):
        raise WheelCheckError("native wheel no longer matches its receipt")
    return wheel


def repair(args: argparse.Namespace) -> tuple[dict[str, Any], dict[str, Any]]:
    source_receipt = load_json(args.receipt.resolve())
    source_wheel = validate_source_receipt(
        source_receipt, args.wheel_dir.resolve()
    )
    output_dir = args.output_dir.resolve()
    output_dir.mkdir(parents=True, exist_ok=True)
    if list(output_dir.glob("*.whl")):
        raise WheelCheckError(f"output directory already contains a wheel: {output_dir}")

    version = run((args.auditwheel, "--version"))
    before = run((args.auditwheel, "show", str(source_wheel)))
    with tempfile.TemporaryDirectory(prefix=".postgamma-repair-", dir=output_dir) as temp:
        stage = Path(temp)
        run(
            (
                args.auditwheel,
                "repair",
                "--plat",
                args.platform,
                "--wheel-dir",
                str(stage),
                str(source_wheel),
            )
        )
        repaired = sorted(stage.glob("*.whl"))
        if len(repaired) != 1:
            raise WheelCheckError(
                f"auditwheel produced {len(repaired)} wheel(s), expected one"
            )
        destination = output_dir / repaired[0].name
        if destination.exists():
            raise WheelCheckError(f"repaired wheel already exists: {destination}")
        os.replace(repaired[0], destination)

    tags = wheel_tags(destination)
    after = run((args.auditwheel, "show", str(destination)))
    content = destination.read_bytes()
    receipt = {
        "schema_version": 2,
        "kind": RECEIPT_KIND,
        "name": source_receipt["name"],
        "version": source_receipt["version"],
        "filename": destination.name,
        "sha256": hashlib.sha256(content).hexdigest(),
        "size": len(content),
        "tags": tags,
        "platform_policy": args.platform,
        "kernel_linkage": source_receipt.get("kernel_linkage"),
        "bundled_kernel_library": source_receipt.get("bundled_kernel_library"),
        "kernel_archive_sha256": source_receipt.get("kernel_archive_sha256"),
        "python": source_receipt.get("python"),
        "metadata_version": source_receipt.get("metadata_version"),
        "requires_python": source_receipt.get("requires_python"),
        "license_files": source_receipt.get("license_files"),
    }
    report = {
        "schema_version": 1,
        "kind": REPORT_KIND,
        "status": "pass",
        "policy": args.platform,
        "auditwheel_version": version.stdout.strip() or version.stderr.strip(),
        "source": {
            "filename": source_wheel.name,
            "sha256": source_receipt["sha256"],
        },
        "repaired": {
            "filename": destination.name,
            "sha256": receipt["sha256"],
            "tags": tags,
        },
        "audit": {
            "before_sha256": hashlib.sha256(before.stdout.encode()).hexdigest(),
            "after_sha256": hashlib.sha256(after.stdout.encode()).hexdigest(),
        },
    }
    return receipt, report


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--wheel-dir", required=True, type=Path)
    parser.add_argument("--receipt", required=True, type=Path)
    parser.add_argument("--output-dir", required=True, type=Path)
    parser.add_argument("--output-receipt", required=True, type=Path)
    parser.add_argument("--output-report", required=True, type=Path)
    parser.add_argument("--platform", default="manylinux_2_28_x86_64")
    parser.add_argument("--auditwheel", default="auditwheel")
    args = parser.parse_args()
    try:
        receipt, report = repair(args)
    except (OSError, WheelCheckError) as error:
        parser.error(str(error))
    for path, document in (
        (args.output_receipt, receipt),
        (args.output_report, report),
    ):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(
            json.dumps(document, indent=2, sort_keys=True) + "\n", encoding="utf-8"
        )
    print(args.output_dir.resolve() / receipt["filename"])
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
