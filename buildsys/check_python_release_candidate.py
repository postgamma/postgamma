#!/usr/bin/env python3
"""Close the local Python release-candidate evidence graph."""

from __future__ import annotations

import argparse
import ast
import hashlib
import json
import re
from pathlib import Path
from typing import Any

from check_python_wheel import (
    WheelCheckError,
    load_json,
    load_release_baseline,
    release_license_files,
)


EVIDENCE_KIND = "postgamma.python-release-candidate"


class PythonReleaseCheckError(RuntimeError):
    """The local Python release candidate is incomplete."""


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def literal_assignments(path: Path) -> dict[str, Any]:
    tree = ast.parse(path.read_text(encoding="utf-8"), filename=str(path))
    values: dict[str, Any] = {}
    for statement in tree.body:
        if not isinstance(statement, ast.Assign) or len(statement.targets) != 1:
            continue
        target = statement.targets[0]
        if not isinstance(target, ast.Name):
            continue
        try:
            values[target.id] = ast.literal_eval(statement.value)
        except (TypeError, ValueError):
            continue
    return values


def keyword_default(path: Path, class_name: str, method_name: str, name: str) -> Any:
    tree = ast.parse(path.read_text(encoding="utf-8"), filename=str(path))
    for statement in tree.body:
        if not isinstance(statement, ast.ClassDef) or statement.name != class_name:
            continue
        for member in statement.body:
            if not isinstance(member, ast.FunctionDef) or member.name != method_name:
                continue
            for argument, default in zip(
                member.args.kwonlyargs, member.args.kw_defaults, strict=True
            ):
                if argument.arg == name and default is not None:
                    return ast.literal_eval(default)
    raise PythonReleaseCheckError(
        f"cannot find {class_name}.{method_name} keyword default: {name}"
    )


def effective_limits(root: Path) -> dict[str, int]:
    source = (root / "embedded-c/src/postgamma.c").read_text(encoding="utf-8")

    def mib_macro(name: str) -> int:
        match = re.search(
            rf"^#define {name} \(([0-9]+)U \* 1024U \* 1024U\)$",
            source,
            re.MULTILINE,
        )
        if match is None:
            raise PythonReleaseCheckError(f"cannot find embedded limit macro: {name}")
        return int(match.group(1)) * 1024 * 1024

    return {
        "default_result_bytes": mib_macro("PGM_DEFAULT_RESULT_BUFFER_LIMIT"),
        "default_value_bytes": mib_macro("PGM_DEFAULT_MAXIMUM_VALUE_SIZE"),
        "default_worker_count": keyword_default(
            root / "python/src/postgamma/_api.py",
            "Database",
            "__init__",
            "worker_count",
        ),
    }


def require_document(
    document: dict[str, Any], kind: str, label: str
) -> dict[str, Any]:
    if document.get("kind") != kind or document.get("status") != "pass":
        raise PythonReleaseCheckError(f"{label} evidence is not a passing {kind}")
    return document


def same_unique_strings(value: Any, expected: list[str]) -> bool:
    """Compare an unordered metadata field without accepting duplicates."""
    return (
        isinstance(value, list)
        and all(isinstance(item, str) for item in value)
        and len(value) == len(set(value))
        and sorted(value) == sorted(expected)
    )


def validate_source(
    root: Path, baseline: dict[str, Any]
) -> dict[str, Any]:
    package = root / "python/src/postgamma"
    exports = literal_assignments(package / "__init__.py")
    version = (root / "VERSION").read_text(encoding="ascii").strip()
    dbapi = {
        name: exports.get(name)
        for name in ("apilevel", "threadsafety", "paramstyle")
    }
    if version != baseline["release"]["version"]:
        raise PythonReleaseCheckError(
            "Python source version differs from the release baseline"
        )
    if exports.get("__all__") != baseline["public_exports"]:
        raise PythonReleaseCheckError(
            "Python source exports differ from the release baseline"
        )
    if dbapi != baseline["dbapi"]:
        raise PythonReleaseCheckError(
            "DB-API constants differ from the release baseline"
        )
    backend = literal_assignments(root / "python/build_backend.py")
    expected_backend = {
        "NAME": baseline["release"]["name"],
        "KERNEL_ARCHIVE_NAME": baseline["wheel"]["build_kernel_archive"],
        "METADATA_VERSION": baseline["wheel"]["metadata_version"],
        "REQUIRES_PYTHON": baseline["python"]["requires_python"],
        "DESCRIPTION_CONTENT_TYPE": baseline["wheel"]["description_content_type"],
        "LICENSE_EXPRESSION": baseline["wheel"]["license_expression"],
        "PROJECT_URL": baseline["wheel"]["project_urls"]["Homepage"],
    }
    for name, expected in expected_backend.items():
        if backend.get(name) != expected:
            raise PythonReleaseCheckError(f"Python build metadata changed: {name}")
    readme = root / "python/README.md"
    if len(readme.read_text(encoding="utf-8").strip()) < 400:
        raise PythonReleaseCheckError("Python package description is incomplete")
    project_licenses = baseline["wheel"]["project_license"]
    for name, expected in project_licenses.items():
        path = root / name
        if not path.is_file() or sha256(path) != expected:
            raise PythonReleaseCheckError(f"project license changed: {name}")
    third_party_licenses = baseline["wheel"]["third_party_licenses"]
    third_party_paths = {
        "THIRD_PARTY_NOTICES": root / "THIRD_PARTY_NOTICES",
        "LICENSE.postgresql": root / "licenses/LICENSE.postgresql",
        "LICENSE.pgvector": root / "licenses/LICENSE.pgvector",
    }
    if set(third_party_licenses) != set(third_party_paths):
        raise PythonReleaseCheckError("third-party license inventory changed")
    for name, expected in third_party_licenses.items():
        path = third_party_paths[name]
        if not path.is_file() or sha256(path) != expected:
            raise PythonReleaseCheckError(f"third-party license changed: {name}")
    limits = effective_limits(root)
    if limits != baseline["limits"]:
        raise PythonReleaseCheckError(
            "Python product limits differ from the release baseline"
        )
    return {
        "version": version,
        "public_api_version": baseline["release"]["public_api_version"],
        "public_export_count": len(baseline["public_exports"]),
        "dbapi": dbapi,
        "description_sha256": sha256(readme),
        "license_expression": baseline["wheel"]["license_expression"],
        "project_license": project_licenses,
        "third_party_licenses": third_party_licenses,
        "limits": limits,
    }


def close_release(
    root: Path,
    baseline_path: Path,
    embedded_release_path: Path,
    receipt_path: Path,
    wheel_path: Path,
    workflow_path: Path,
    portability_path: Path,
) -> dict[str, Any]:
    baseline = load_release_baseline(baseline_path)
    baseline_hash = sha256(baseline_path)
    source = validate_source(root, baseline)
    embedded_release = require_document(
        load_json(embedded_release_path),
        "postgamma.embedded-release",
        "embedded release",
    )
    if (
        embedded_release.get("postgresql_major")
        != baseline["postgresql"]["major"]
        or embedded_release.get("abi", {}).get("encoded_version")
        != baseline["postgresql"]["abi_version"]
        or embedded_release.get("abi", {}).get("advertised_capabilities")
        != baseline["postgresql"]["capabilities"]
        or "static"
        not in embedded_release.get("product", {}).get("delivery_forms", [])
    ):
        raise PythonReleaseCheckError(
            "embedded kernel identity differs from the Python release baseline"
        )

    receipt = load_json(receipt_path)
    if (
        receipt.get("schema_version") != 2
        or receipt.get("kind") != "postgamma.python-wheel-build"
        or receipt.get("name") != baseline["release"]["name"]
        or receipt.get("version") != baseline["release"]["version"]
        or receipt.get("platform_policy") != "linux_native"
        or receipt.get("kernel_linkage") != "static"
        or receipt.get("bundled_kernel_library") is not False
        or not isinstance(receipt.get("kernel_archive_sha256"), str)
        or re.fullmatch(r"[0-9a-f]{64}", receipt["kernel_archive_sha256"])
        is None
    ):
        raise PythonReleaseCheckError(
            "local wheel receipt differs from the Python release baseline"
        )
    wheel = require_document(
        load_json(wheel_path), "postgamma.python-wheel-evidence", "Python wheel"
    )
    gates = wheel.get("gates")
    if not isinstance(gates, dict):
        raise PythonReleaseCheckError("Python wheel gate evidence is missing")
    expected_public_api = {
        "export_count": len(baseline["public_exports"]),
        "baseline_sha256": baseline_hash,
    }
    if (
        wheel.get("wheel", {}).get("sha256") != receipt.get("sha256")
        or wheel.get("release_baseline", {}).get("sha256") != baseline_hash
        or gates.get("installation")
        != {"mode": "archive", "pip_returncode": None}
        or gates.get("public_api") != expected_public_api
        or wheel.get("kernel")
        != {
            "linkage": "static",
            "native_dynamic_exports": [
                baseline["wheel"]["native_dynamic_export"]
            ],
            "postgamma_dynamic_dependencies": 0,
            "separate_library": False,
        }
        or not same_unique_strings(
            wheel.get("metadata", {}).get("license_files"),
            list(release_license_files(baseline)),
        )
    ):
        raise PythonReleaseCheckError("local wheel is not an isolated release artifact")

    workflow = require_document(
        load_json(workflow_path),
        "postgamma.python-release-workflow-contract",
        "manylinux workflow",
    )
    if (
        workflow.get("release_baseline_sha256") != baseline_hash
        or workflow.get("python_tags") != baseline["python"]["tags"]
        or workflow.get("manylinux_policy")
        != baseline["python"]["manylinux_policy"]
        or workflow.get("publish_authorized") is not False
    ):
        raise PythonReleaseCheckError(
            "manylinux workflow differs from the Python release baseline"
        )

    portability = require_document(
        load_json(portability_path),
        "postgamma.portability-evidence",
        "source portability",
    )
    if portability.get("developer_path_violations") != 0:
        raise PythonReleaseCheckError("source portability evidence is incomplete")

    inputs = {
        "baseline": baseline_hash,
        "embedded_release": sha256(embedded_release_path),
        "wheel_receipt": sha256(receipt_path),
        "wheel_evidence": sha256(wheel_path),
        "workflow_contract": sha256(workflow_path),
        "portability": sha256(portability_path),
    }
    return {
        "schema_version": 1,
        "kind": EVIDENCE_KIND,
        "status": "pass",
        "release": baseline["release"],
        "postgresql": baseline["postgresql"],
        "source": source,
        "local_wheel": {
            "filename": wheel["wheel"]["filename"],
            "sha256": wheel["wheel"]["sha256"],
            "tags": wheel["wheel"]["tags"],
            "kernel_linkage": "static",
            "separate_kernel_library": False,
            "kernel_archive_sha256": receipt["kernel_archive_sha256"],
            "isolated_archive_executed": True,
            "clean_pip_installed": False,
        },
        "declared_manylinux_matrix": {
            "python_tags": workflow["python_tags"],
            "platform_policy": workflow["manylinux_policy"],
            "execution_evidence_kind": workflow["execution_evidence_required"],
            "executed_by_local_gate": False,
        },
        "seal": {
            "code_sealed": True,
            "project_license_declared": True,
            "public_release_ready": False,
            "remaining_owner_decision": "publish-authorization",
            "remaining_external_gate": "postgamma.python-release-evidence",
            "publish_authorized": False,
        },
        "inputs": inputs,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", required=True, type=Path)
    parser.add_argument("--baseline", required=True, type=Path)
    parser.add_argument("--embedded-release", required=True, type=Path)
    parser.add_argument("--wheel-receipt", required=True, type=Path)
    parser.add_argument("--wheel-evidence", required=True, type=Path)
    parser.add_argument("--workflow-contract", required=True, type=Path)
    parser.add_argument("--portability", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    try:
        report = close_release(
            args.root.resolve(),
            args.baseline.resolve(),
            args.embedded_release.resolve(),
            args.wheel_receipt.resolve(),
            args.wheel_evidence.resolve(),
            args.workflow_contract.resolve(),
            args.portability.resolve(),
        )
    except (OSError, ValueError, WheelCheckError, PythonReleaseCheckError) as error:
        parser.error(str(error))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(
        json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    print(
        "Python release candidate: code and Apache-2.0 license sealed; "
        "manylinux execution and publish authorization remain external"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
