#!/usr/bin/env python3
"""Derive the bootstrap PostgreSQL kernel link closure from a captured link command."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import shlex
import tempfile
from pathlib import Path
from typing import Any

from merge_link_db import DATABASE_KIND, sha256
from postgresql_embedded_adapter import (
    EmbeddedAdapterError,
    load_document as load_embedded_adapter_document,
    validate_adapter,
)


REPORT_KIND = "postgamma.kernel-link-report"


class KernelLinkError(ValueError):
    """The captured backend link cannot produce one unambiguous kernel closure."""


def load_json(path: Path) -> dict[str, Any]:
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise KernelLinkError(f"cannot read {path}: {exc}") from exc
    if not isinstance(document, dict):
        raise KernelLinkError(f"{path}: top-level value must be an object")
    return document


def _relative_or_absolute(path: Path, build_root: Path) -> dict[str, str]:
    try:
        return {"origin": "postgresql-build", "path": path.relative_to(build_root).as_posix()}
    except ValueError:
        return {"origin": "external", "path": str(path)}


def _is_captured_runtime_path(argument: str) -> bool:
    if argument.startswith(("-R", "-Wl,-R,")):
        return True
    if not argument.startswith("-Wl,"):
        return False
    linker_arguments = argument.removeprefix("-Wl,").split(",")
    return any(value in {"-rpath", "--rpath"} for value in linker_arguments)


def _without_output_exclusions_and_runtime_paths(
    arguments: list[str], excluded_argv_indexes: set[int]
) -> tuple[list[str], list[dict[str, Any]]]:
    result: list[str] = []
    omitted: list[dict[str, Any]] = []
    index = 1
    while index < len(arguments):
        argument = arguments[index]
        if index in excluded_argv_indexes:
            index += 1
            continue
        if argument == "-o":
            if index + 1 >= len(arguments):
                raise KernelLinkError("backend link has -o without an output path")
            index += 2
            continue
        if argument.startswith("-o") and len(argument) > 2:
            index += 1
            continue
        if _is_captured_runtime_path(argument):
            omitted.append(
                {
                    "argv_index": index,
                    "argument": argument,
                    "reason": "captured-install-prefix-is-not-part-of-embedded-runtime",
                }
            )
            index += 1
            continue
        result.append(argument)
        index += 1
    return result, omitted


def response_content(arguments: list[str]) -> str:
    return "".join(shlex.quote(argument) + "\n" for argument in arguments)


def _matches_suffix(path: str, suffix: str) -> bool:
    normalized = path.replace("\\", "/")
    return normalized == suffix or normalized.endswith("/" + suffix)


def derive(
    database: dict[str, Any],
    build_root: Path,
    configure_state: Path,
    config_log: Path,
    upstream_manifest: Path,
    embedded_adapter: dict[str, Any],
) -> tuple[list[str], dict[str, Any]]:
    adapter = validate_adapter(embedded_adapter)
    if database.get("schema_version") != 1 or database.get("kind") != DATABASE_KIND:
        raise KernelLinkError(
            f"link database must use schema_version 1 and kind {DATABASE_KIND}"
        )
    if database.get("build_root") != str(build_root):
        raise KernelLinkError("link database belongs to a different build root")
    identity = database.get("identity")
    if not isinstance(identity, dict) or identity.get(
        "configure_state_sha256"
    ) != sha256(configure_state):
        raise KernelLinkError("link database configure identity is stale")
    if identity.get("toolchain_config_log_sha256") != sha256(config_log):
        raise KernelLinkError("link database toolchain identity is stale")
    commands = database.get("commands")
    if not isinstance(commands, list):
        raise KernelLinkError("link database commands must be an array")
    backend = [
        command
        for command in commands
        if isinstance(command, dict)
        and command.get("target") == adapter["kernel_link_target"]
    ]
    if len(backend) != 1:
        raise KernelLinkError(
            f"expected one {adapter['kernel_link_target']} link command, found {len(backend)}"
        )
    command = backend[0]
    arguments = command.get("arguments")
    inputs = command.get("ordered_inputs")
    if not isinstance(arguments, list) or not all(
        isinstance(argument, str) for argument in arguments
    ):
        raise KernelLinkError("backend link arguments must be an argv array")
    if not isinstance(inputs, list):
        raise KernelLinkError("backend link ordered_inputs must be an array")

    normalized_inputs: list[dict[str, Any]] = []
    for item in inputs:
        if not isinstance(item, dict):
            raise KernelLinkError("backend link input must be an object")
        path_value = item.get("path")
        kind = item.get("kind")
        argv_index = item.get("argv_index")
        if (
            not isinstance(path_value, str)
            or kind not in {"object", "archive", "response-file"}
            or not isinstance(argv_index, int)
            or not 1 <= argv_index < len(arguments)
        ):
            raise KernelLinkError("backend link input facts are invalid")
        path = Path(path_value).resolve()
        if not path.is_file():
            raise KernelLinkError(f"backend link input no longer exists: {path}")
        normalized_inputs.append({**item, "resolved_path": path})

    excluded_by_index: dict[int, dict[str, Any]] = {}
    for rule in adapter["kernel_exclusions"]:
        matches = [
            item
            for item in normalized_inputs
            if _matches_suffix(item["path"], rule["path_suffix"])
        ]
        if len(matches) != rule["expected_matches"]:
            raise KernelLinkError(
                f"kernel exclusion {rule['id']} expected {rule['expected_matches']} "
                f"match(es), found {len(matches)}"
            )
        for item in matches:
            if item["argv_index"] in excluded_by_index:
                raise KernelLinkError(
                    f"kernel input {item['path']} matches more than one exclusion"
                )
            excluded_by_index[item["argv_index"]] = rule

    roots_by_index: dict[int, dict[str, Any]] = {}
    for rule in adapter["catalog_registration_roots"]:
        matches = [
            item
            for item in normalized_inputs
            if _matches_suffix(item["path"], rule["path_suffix"])
        ]
        if len(matches) != rule["expected_matches"]:
            raise KernelLinkError(
                f"catalog registration root {rule['id']} expected "
                f"{rule['expected_matches']} match(es), found {len(matches)}"
            )
        for item in matches:
            if item["argv_index"] in excluded_by_index:
                raise KernelLinkError(
                    f"catalog registration root {item['path']} is also excluded"
                )
            if item["argv_index"] in roots_by_index:
                raise KernelLinkError(
                    f"kernel input {item['path']} matches more than one registration root"
                )
            roots_by_index[item["argv_index"]] = rule

    included: list[dict[str, Any]] = []
    for item in normalized_inputs:
        if item["argv_index"] in excluded_by_index:
            continue
        path = item["resolved_path"]
        root_rule = roots_by_index.get(item["argv_index"])
        included.append(
            {
                "argv_index": item["argv_index"],
                "kind": item["kind"],
                **_relative_or_absolute(path, build_root),
                "keep_reason": (
                    root_rule["reason"]
                    if root_rule is not None
                    else "captured-postgres-backend-link"
                ),
                "catalog_reachability": (
                    root_rule["id"]
                    if root_rule is not None
                    else "not-a-declared-root"
                ),
            }
        )

    closure, omitted_runtime_paths = _without_output_exclusions_and_runtime_paths(
        arguments, set(excluded_by_index)
    )
    upstream = load_json(upstream_manifest)
    commit = upstream.get("commit")
    if not isinstance(commit, str) or len(commit) != 40:
        raise KernelLinkError("upstream manifest commit is invalid")
    content = response_content(closure)
    report = {
        "schema_version": 1,
        "kind": REPORT_KIND,
        "identity": {
            "upstream_commit": commit,
            "configure_state_sha256": sha256(configure_state),
            "toolchain_config_log_sha256": sha256(config_log),
            "link_database_sha256": None,
            "embedded_adapter_id": adapter["id"],
            "embedded_adapter_sha256": None,
        },
        "source_link": {
            "target": adapter["kernel_link_target"],
            "output": command.get("output"),
            "compiler": arguments[0],
        },
        "excluded": [
            {
                "argv_index": item["argv_index"],
                "kind": item["kind"],
                **_relative_or_absolute(item["resolved_path"], build_root),
                "policy": excluded_by_index[item["argv_index"]]["id"],
                "reason": excluded_by_index[item["argv_index"]]["reason"],
            }
            for item in normalized_inputs
            if item["argv_index"] in excluded_by_index
        ],
        "omitted_runtime_paths": omitted_runtime_paths,
        "included": included,
        "closure_arguments": closure,
        "library_arguments": command.get("library_arguments", []),
        "linker_arguments": command.get("linker_arguments", []),
        "response_sha256": hashlib.sha256(content.encode("utf-8")).hexdigest(),
        "catalog_registration_roots": {
            "status": "matched",
            "objects": [
                {
                    "id": rule["id"],
                    **_relative_or_absolute(item["resolved_path"], build_root),
                    "reason": rule["reason"],
                }
                for item in normalized_inputs
                if (rule := roots_by_index.get(item["argv_index"])) is not None
            ],
        },
        "unresolved_symbols": {
            "status": "not-linked",
            "symbols": [],
        },
        "limitations": [
            "This report reproduces the captured backend link closure but does not "
            "claim a valid shared-library link.",
            "Transitive catalog reachability and unresolved symbols become "
            "executable gates in the static-extension proof.",
        ],
    }
    return closure, report


def write_text(path: Path, content: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.exists() and path.read_text(encoding="utf-8") == content:
        os.utime(path, None)
        return
    with tempfile.NamedTemporaryFile(
        mode="w", encoding="utf-8", dir=path.parent, prefix=f".{path.name}.", delete=False
    ) as handle:
        handle.write(content)
        temporary = Path(handle.name)
    try:
        os.replace(temporary, path)
    finally:
        temporary.unlink(missing_ok=True)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--database", required=True, type=Path)
    parser.add_argument("--build-root", required=True, type=Path)
    parser.add_argument("--configure-state", required=True, type=Path)
    parser.add_argument("--config-log", required=True, type=Path)
    parser.add_argument("--upstream-manifest", required=True, type=Path)
    parser.add_argument("--embedded-adapter", required=True, type=Path)
    parser.add_argument("--response-file", required=True, type=Path)
    parser.add_argument("--report", required=True, type=Path)
    args = parser.parse_args()
    try:
        database_path = args.database.resolve()
        embedded_adapter_path = args.embedded_adapter.resolve()
        closure, report = derive(
            load_json(database_path),
            args.build_root.resolve(),
            args.configure_state.resolve(),
            args.config_log.resolve(),
            args.upstream_manifest.resolve(),
            load_embedded_adapter_document(embedded_adapter_path),
        )
        report["identity"]["link_database_sha256"] = sha256(database_path)
        report["identity"]["embedded_adapter_sha256"] = sha256(
            embedded_adapter_path
        )
        write_text(args.response_file.resolve(), response_content(closure))
        write_text(
            args.report.resolve(),
            json.dumps(report, indent=2, sort_keys=True) + "\n",
        )
    except (EmbeddedAdapterError, KernelLinkError, OSError) as exc:
        parser.error(str(exc))
    print(
        f"kernel link closure: {len(report['included'])} inputs, "
        f"{len(report['excluded'])} excluded process shell input(s) -> {args.report}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
