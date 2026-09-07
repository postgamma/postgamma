#!/usr/bin/env python3
"""Compile the backend-state catalog and policy into source replacements."""

from __future__ import annotations

import argparse
import hashlib
import json
from collections import Counter, defaultdict
from pathlib import Path, PurePosixPath
from typing import Any

from check_backend_state_policy import (
    VIRTUAL_OWNERS,
    catalog_candidates,
    load_json,
    policy_decisions,
)
from generate_backend_state_runtime import relocation_bridge_name


RUNTIME_KIND = "postgamma.backend-state-runtime"


def safe_source(source_root: Path, relative: str) -> Path:
    logical = PurePosixPath(relative)
    if logical.is_absolute() or ".." in logical.parts:
        raise ValueError(f"unsafe catalog path: {relative}")
    path = (source_root / Path(*logical.parts)).resolve()
    try:
        path.relative_to(source_root)
    except ValueError as exc:
        raise ValueError(f"catalog path escapes source root: {relative}") from exc
    if not path.is_file():
        raise ValueError(f"catalog source does not exist: {relative}")
    return path


def byte_offset_for_line_column(source: bytes, line: int, column: int) -> int:
    if line < 1 or column < 1:
        raise ValueError(f"invalid source location {line}:{column}")
    starts = [0]
    for index, value in enumerate(source):
        if value == 10:
            starts.append(index + 1)
    if line > len(starts):
        raise ValueError(f"line {line} is outside source")
    return starts[line - 1] + column - 1


def declaration_terminator(source: bytes, start: int) -> int:
    parentheses = brackets = braces = 0
    quote: int | None = None
    escaped = False
    line_comment = False
    block_comment = False
    index = start
    while index < len(source):
        value = source[index]
        following = source[index + 1] if index + 1 < len(source) else -1
        if line_comment:
            if value == 10:
                line_comment = False
            index += 1
            continue
        if block_comment:
            if value == 42 and following == 47:
                block_comment = False
                index += 2
            else:
                index += 1
            continue
        if quote is not None:
            if escaped:
                escaped = False
            elif value == 92:
                escaped = True
            elif value == quote:
                quote = None
            index += 1
            continue
        if value == 47 and following == 47:
            line_comment = True
            index += 2
            continue
        if value == 47 and following == 42:
            block_comment = True
            index += 2
            continue
        if value in (34, 39):
            quote = value
        elif value == 40:
            parentheses += 1
        elif value == 41:
            parentheses -= 1
        elif value == 91:
            brackets += 1
        elif value == 93:
            brackets -= 1
        elif value == 123:
            braces += 1
        elif value == 125:
            braces -= 1
        elif value == 59 and parentheses == brackets == braces == 0:
            return index + 1
        if min(parentheses, brackets, braces) < 0:
            break
        index += 1
    raise ValueError("cannot locate declaration terminator")


def relocation_bridge(
    owner: dict[str, Any], owner_runtime: dict[str, Any], runtime_by_id: dict[str, Any]
) -> str:
    function = relocation_bridge_name(owner["id"])
    targets = [runtime_by_id[identifier] for identifier in owner_runtime["relocations"]]
    lines = [
        "",
        "",
        "#ifndef FRONTEND",
        "void *",
        f"{function}(void)",
        "{",
        "\tstatic const PostgammaBackendStateTemplate targets[] =",
        "\t{",
    ]
    for target in targets:
        lines.append(f"\t\t{{{target['enum']}, &({target['name']})}},")
    lines.extend(
        [
            "\t};",
            "",
            "\treturn postgamma_backend_state_address_relocated(",
            f"\t\t{owner_runtime['enum']}, &({owner['name']}), targets,",
            "\t\tsizeof(targets) / sizeof(targets[0]));",
            "}",
            "#endif /* FRONTEND */",
        ]
    )
    return "\n".join(lines)


def compile_plan(
    catalog: dict[str, Any],
    policy: dict[str, Any],
    runtime: dict[str, Any],
    artifact_root: Path,
    source_kind: str = "source",
) -> dict[str, Any]:
    candidates = catalog_candidates(catalog)
    decisions = policy_decisions(policy)
    if runtime.get("schema_version") != 1 or runtime.get("kind") != RUNTIME_KIND:
        raise ValueError(f"runtime: expected schema_version 1 and kind {RUNTIME_KIND}")
    runtime_slots = runtime.get("slots")
    if not isinstance(runtime_slots, list):
        raise ValueError("runtime: slots must be an array")
    runtime_by_id = {entry["id"]: entry for entry in runtime_slots}
    virtual_ids = {
        identifier
        for identifier in candidates
        if decisions[identifier]["owner"] in VIRTUAL_OWNERS
    }
    if set(runtime_by_id) != virtual_ids:
        raise ValueError("runtime slot inventory does not match virtual ownership policy")

    file_replacements: dict[str, dict[tuple[int, int], dict[str, Any]]] = defaultdict(dict)
    uses_by_kind: Counter[str] = Counter()
    skipped_static = 0
    initializer_classes: dict[tuple[str, int, int], set[bool]] = defaultdict(set)
    for index, use in enumerate(catalog.get("uses", [])):
        if not isinstance(use, dict):
            raise ValueError(f"catalog: uses[{index}] must be an object")
        if use.get("id") not in virtual_ids:
            continue
        if use.get("source_kind", "source") != source_kind:
            continue
        path = use.get("path")
        offset = use.get("offset")
        length = use.get("length")
        if not isinstance(path, str) or not isinstance(offset, int) or not isinstance(length, int):
            raise ValueError(f"catalog: uses[{index}] has invalid source location")
        initializer_classes[(path, offset, length)].add(
            bool(use.get("in_static_initializer"))
        )
    ambiguous = sorted(
        location for location, classes in initializer_classes.items() if len(classes) > 1
    )
    if ambiguous:
        path, offset, length = ambiguous[0]
        raise ValueError(
            "source token is classified as both static-initializer and runtime use: "
            f"{source_kind}:{path}:{offset}+{length}"
        )

    for index, use in enumerate(catalog.get("uses", [])):
        if not isinstance(use, dict):
            raise ValueError(f"catalog: uses[{index}] must be an object")
        identifier = use.get("id")
        if identifier not in virtual_ids:
            continue
        if use.get("source_kind", "source") != source_kind:
            continue
        if use.get("in_static_initializer"):
            skipped_static += 1
            continue
        path = use.get("path")
        offset = use.get("offset")
        length = use.get("length")
        if not isinstance(path, str) or not isinstance(offset, int) or not isinstance(length, int):
            raise ValueError(f"catalog: uses[{index}] has invalid source location")
        candidate = candidates[identifier]
        runtime_slot = runtime_by_id[identifier]
        name = candidate["name"]
        if runtime_slot["relocations"]:
            replacement_text = (
                f"POSTGAMMA_BACKEND_STATE_RELOCATED_VALUE({name}, "
                f"{relocation_bridge_name(identifier)})"
            )
        else:
            replacement_text = (
                f"POSTGAMMA_BACKEND_STATE_VALUE({runtime_slot['enum']}, {name})"
            )
        recorded_use_kind = (
            "macro_body" if use.get("macro_kind") == "body" else use.get("use_kind", "read")
        )
        replacement = {
            "offset": offset,
            "length": length,
            "original": name,
            "replacement": replacement_text,
            "kind": "backend_state",
            "rule_id": identifier,
            "use_kind": recorded_use_kind,
            "line": use.get("line", 0),
            "column": use.get("column", 0),
        }
        key = (offset, length)
        existing = file_replacements[path].get(key)
        if existing is not None:
            semantic_fields = ("original", "replacement", "rule_id")
            if any(existing[field] != replacement[field] for field in semantic_fields):
                raise ValueError(
                    f"inconsistent semantic binding at {path}:{offset}+{length}: "
                    f"{existing['rule_id']} versus {identifier}"
                )
            if existing["use_kind"] != replacement["use_kind"]:
                existing["use_kind"] = "mixed"
            continue
        file_replacements[path][key] = replacement
        uses_by_kind[recorded_use_kind] += 1

    for identifier in sorted(virtual_ids):
        runtime_slot = runtime_by_id[identifier]
        if not runtime_slot["relocations"]:
            continue
        candidate = candidates[identifier]
        if candidate.get("definition_source_kind", "source") != source_kind:
            continue
        path = candidate["definition_path"]
        source_path = safe_source(artifact_root, path)
        source = source_path.read_bytes()
        name_offset = byte_offset_for_line_column(
            source, int(candidate["line"]), int(candidate["column"])
        )
        name = candidate["name"].encode("utf-8")
        if source[name_offset : name_offset + len(name)] != name:
            raise ValueError(
                f"definition spelling changed for {identifier} at {path}:"
                f"{candidate['line']}:{candidate['column']}"
            )
        insertion_offset = declaration_terminator(source, name_offset + len(name))
        bridge = relocation_bridge(candidate, runtime_slot, runtime_by_id)
        replacement = {
            "offset": insertion_offset,
            "length": 0,
            "original": "",
            "replacement": bridge,
            "kind": "backend_relocation_bridge",
            "rule_id": identifier,
            "use_kind": "injection",
            "line": candidate["line"],
            "column": candidate["column"],
        }
        key = (insertion_offset, 0)
        if key in file_replacements[path]:
            raise ValueError(f"relocation bridge insertion collision at {path}:{insertion_offset}")
        file_replacements[path][key] = replacement

    files = []
    replacement_count = 0
    for path in sorted(file_replacements):
        source = safe_source(artifact_root, path).read_bytes()
        replacements = sorted(
            file_replacements[path].values(),
            key=lambda item: (item["offset"], item["length"], item["rule_id"]),
        )
        previous_end = -1
        for replacement in replacements:
            start = replacement["offset"]
            end = start + replacement["length"]
            if start < previous_end:
                raise ValueError(f"overlapping backend replacements in {path} near {start}")
            expected = replacement["original"].encode("utf-8")
            if source[start:end] != expected:
                raise ValueError(
                    f"source spelling mismatch in {path} at {start}: "
                    f"expected {expected!r}, found {source[start:end]!r}"
                )
            previous_end = end
        replacement_count += len(replacements)
        files.append(
            {
                "path": path,
                "sha256": hashlib.sha256(source).hexdigest(),
                "replacements": replacements,
            }
        )
    return {
        "schema_version": 1,
        "mode": "plan",
        "kind": (
            "postgamma.backend-state-plan"
            if source_kind == "source"
            else "postgamma.backend-generated-state-plan"
        ),
        "source_root": ".",
        "files": files,
        "summary": {
            "virtual_candidate_count": len(virtual_ids),
            "file_count": len(files),
            "replacement_count": replacement_count,
            "skipped_static_initializer_use_count": skipped_static,
            "relocation_bridge_count": sum(
                bool(runtime_by_id[identifier]["relocations"])
                and candidates[identifier].get("definition_source_kind", "source")
                == source_kind
                for identifier in virtual_ids
            ),
            "by_kind": dict(sorted(uses_by_kind.items())),
        },
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--catalog", required=True, type=Path)
    parser.add_argument("--policy", required=True, type=Path)
    parser.add_argument("--runtime", required=True, type=Path)
    parser.add_argument("--source-root", required=True, type=Path)
    parser.add_argument("--generated-root", type=Path)
    parser.add_argument("--generated-output", type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    if bool(args.generated_root) != bool(args.generated_output):
        parser.error("--generated-root and --generated-output must be used together")
    try:
        plan = compile_plan(
            load_json(args.catalog),
            load_json(args.policy),
            load_json(args.runtime),
            args.source_root.resolve(),
            "source",
        )
    except (ValueError, KeyError, TypeError) as exc:
        parser.error(str(exc))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(
        json.dumps(plan, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    summary = plan["summary"]
    print(
        f"backend state plan: {summary['virtual_candidate_count']} candidates, "
        f"{summary['replacement_count']} replacements in {summary['file_count']} files, "
        f"{summary['relocation_bridge_count']} relocation bridges"
    )
    if args.generated_root and args.generated_output:
        try:
            generated_plan = compile_plan(
                load_json(args.catalog),
                load_json(args.policy),
                load_json(args.runtime),
                args.generated_root.resolve(),
                "generated_build",
            )
        except (ValueError, KeyError, TypeError) as exc:
            parser.error(str(exc))
        args.generated_output.parent.mkdir(parents=True, exist_ok=True)
        args.generated_output.write_text(
            json.dumps(generated_plan, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
        generated_summary = generated_plan["summary"]
        print(
            "generated backend state plan: "
            f"{generated_summary['replacement_count']} replacements in "
            f"{generated_summary['file_count']} files"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
