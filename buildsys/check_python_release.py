#!/usr/bin/env python3
"""Close the complete manylinux wheel matrix without publishing it."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
from typing import Any, Sequence

from check_python_wheel import (
    EXPECTED_SCENARIOS,
    WheelCheckError,
    load_json,
    load_release_baseline,
    release_license_files,
)


REPORT_KIND = "postgamma.python-release-evidence"
REQUIRED_GATE_NAMES = {
    "arrow_interop",
    "asyncio_waitables",
    "bundled_pgvector",
    "copy_streaming",
    "documentation_examples",
    "event_routing",
    "fork_invalidation",
    "finalizer_cleanup",
    "gil_parallelism",
    "installation",
    "integration",
    "management_and_logical",
    "no_build_source_paths",
    "shared_lifecycle",
    "public_api",
    "runtime_closure",
    "streaming_and_prepared",
    "timeout_and_cancel",
    "transactions_and_diagnostics",
    "typed_round_trip",
    "unit",
}


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def python_version_for_tag(tag: str) -> str:
    if not tag.startswith("cp") or not tag[2:].isdigit() or len(tag[2:]) < 2:
        raise WheelCheckError(f"invalid CPython tag: {tag}")
    digits = tag[2:]
    return f"{digits[0]}.{digits[1:]}"


def validate_entry(
    root: Path,
    tag: str,
    platform: str,
    baseline: dict[str, Any],
    baseline_sha256: str,
) -> dict[str, Any]:
    entry = root / tag
    if not entry.is_dir():
        entry = root / f"python-wheel-{tag}"
    receipt_path = entry / "reports/repaired-wheel.json"
    repair_path = entry / "reports/manylinux-repair.json"
    evidence_path = entry / "reports/python-wheel.json"
    receipt = load_json(receipt_path)
    repair = load_json(repair_path)
    evidence = load_json(evidence_path)
    if (
        receipt.get("schema_version") != 2
        or receipt.get("kind") != "postgamma.python-wheel-build"
        or receipt.get("platform_policy") != platform
        or receipt.get("name") != baseline["release"]["name"]
        or receipt.get("version") != baseline["release"]["version"]
        or receipt.get("metadata_version") != baseline["wheel"]["metadata_version"]
        or receipt.get("requires_python") != baseline["python"]["requires_python"]
        or receipt.get("project_url")
        != baseline["wheel"]["project_urls"]["Homepage"]
        or receipt.get("kernel_linkage") != "static"
        or receipt.get("bundled_kernel_library") is not False
        or not isinstance(receipt.get("kernel_archive_sha256"), str)
        or len(receipt["kernel_archive_sha256"]) != 64
        or any(
            character not in "0123456789abcdef"
            for character in receipt["kernel_archive_sha256"]
        )
        or receipt.get("license_files")
        != sorted(release_license_files(baseline))
    ):
        raise WheelCheckError(f"{tag}: invalid repaired-wheel receipt")
    tags = receipt.get("tags")
    if not isinstance(tags, list) or not tags or not all(
        isinstance(item, str) and item.startswith(tag + "-") for item in tags
    ):
        raise WheelCheckError(f"{tag}: wheel tags do not match the matrix entry")
    expected_version = python_version_for_tag(tag)
    if not str(receipt.get("python", "")).startswith(expected_version + "."):
        raise WheelCheckError(f"{tag}: interpreter version does not match its tag")
    wheel = entry / "wheel" / str(receipt.get("filename", ""))
    if not wheel.is_file() or sha256(wheel) != receipt.get("sha256"):
        raise WheelCheckError(f"{tag}: repaired wheel does not match its receipt")
    if (
        repair.get("kind") != "postgamma.python-manylinux-repair"
        or repair.get("status") != "pass"
        or repair.get("policy") != platform
        or repair.get("repaired", {}).get("sha256") != receipt.get("sha256")
    ):
        raise WheelCheckError(f"{tag}: auditwheel repair evidence is incomplete")
    if (
        evidence.get("schema_version") != 2
        or evidence.get("kind") != "postgamma.python-wheel-evidence"
        or evidence.get("status") != "pass"
        or evidence.get("wheel", {}).get("sha256") != receipt.get("sha256")
        or evidence.get("release_baseline", {}).get("sha256") != baseline_sha256
        or evidence.get("kernel", {}).get("linkage") != "static"
        or evidence.get("kernel", {}).get("separate_library") is not False
        or evidence.get("kernel", {}).get("postgamma_dynamic_dependencies") != 0
        or evidence.get("kernel", {}).get("native_dynamic_exports")
        != [baseline["wheel"]["native_dynamic_export"]]
    ):
        raise WheelCheckError(f"{tag}: wheel execution evidence is incomplete")
    gates = evidence.get("gates")
    if not isinstance(gates, dict) or set(gates) != REQUIRED_GATE_NAMES:
        raise WheelCheckError(f"{tag}: wheel evidence gate set is incomplete")
    installation = gates.get("installation")
    if not isinstance(installation, dict) or installation != {
        "mode": "pip",
        "pip_returncode": 0,
    }:
        raise WheelCheckError(f"{tag}: wheel was not tested through clean pip install")
    measured_scenarios = {
        scenario
        for gate in gates.values()
        if isinstance(gate, dict)
        for scenario in gate.get("scenarios", [])
        if isinstance(scenario, str)
    }
    if measured_scenarios != set(EXPECTED_SCENARIOS):
        raise WheelCheckError(f"{tag}: required runtime scenarios are incomplete")
    public_api = gates.get("public_api")
    if not isinstance(public_api, dict) or public_api != {
        "export_count": len(baseline["public_exports"]),
        "baseline_sha256": baseline_sha256,
    }:
        raise WheelCheckError(f"{tag}: public API baseline evidence is incomplete")
    documentation_examples = gates.get("documentation_examples")
    expected_example_names = baseline["documentation"]["examples"]
    stdout_hashes = (
        documentation_examples.get("stdout_sha256")
        if isinstance(documentation_examples, dict)
        else None
    )
    if (
        not isinstance(documentation_examples, dict)
        or documentation_examples.get("count") != len(expected_example_names)
        or documentation_examples.get("names") != expected_example_names
        or not isinstance(stdout_hashes, dict)
        or set(stdout_hashes) != set(expected_example_names)
    ):
        raise WheelCheckError(
            f"{tag}: documentation example execution evidence is incomplete"
        )
    return {
        "python_tag": tag,
        "python": receipt["python"],
        "filename": wheel.name,
        "sha256": receipt["sha256"],
        "tags": tags,
        "member_count": evidence["wheel"]["member_count"],
        "kernel_linkage": "static",
        "separate_kernel_library": False,
        "kernel_archive_sha256": receipt["kernel_archive_sha256"],
    }


def collect(
    root: Path,
    expected_tags: Sequence[str],
    platform: str,
    baseline: dict[str, Any],
    baseline_sha256: str,
) -> dict[str, Any]:
    if len(set(expected_tags)) != len(expected_tags):
        raise WheelCheckError("duplicate expected Python tag")
    unexpected = sorted(
        path.name
        for path in root.iterdir()
        if path.is_dir()
        and path.name not in set(expected_tags)
        and path.name not in {f"python-wheel-{tag}" for tag in expected_tags}
    )
    if unexpected:
        raise WheelCheckError(f"unexpected release matrix entries: {unexpected}")
    if list(expected_tags) != baseline["python"]["tags"]:
        raise WheelCheckError("release matrix differs from the Python baseline")
    if platform != baseline["python"]["manylinux_policy"]:
        raise WheelCheckError("release platform differs from the Python baseline")
    entries = [
        validate_entry(root, tag, platform, baseline, baseline_sha256)
        for tag in expected_tags
    ]
    return {
        "schema_version": 1,
        "kind": REPORT_KIND,
        "status": "pass",
        "platform_policy": platform,
        "python_tags": list(expected_tags),
        "wheel_count": len(entries),
        "wheels": entries,
        "release": baseline["release"],
        "postgresql": baseline["postgresql"],
        "release_baseline_sha256": baseline_sha256,
        "code_sealed": True,
        "distribution_matrix_verified": True,
        "project_license_declared": True,
        "public_release_ready": True,
        "publish_authorized": False,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--artifacts-root", required=True, type=Path)
    parser.add_argument("--baseline", required=True, type=Path)
    parser.add_argument("--python-tag", action="append", required=True)
    parser.add_argument("--platform", default="manylinux_2_28_x86_64")
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    try:
        baseline_path = args.baseline.resolve()
        baseline = load_release_baseline(baseline_path)
        report = collect(
            args.artifacts_root.resolve(),
            args.python_tag,
            args.platform,
            baseline,
            sha256(baseline_path),
        )
    except (OSError, WheelCheckError) as error:
        parser.error(str(error))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(
        json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    print(
        f"Python release matrix: {report['wheel_count']} clean-installed "
        f"{report['platform_policy']} wheels passed"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
