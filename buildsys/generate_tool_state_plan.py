#!/usr/bin/env python3
"""Compile frontend-tool ownership into source-level state replacements."""

from __future__ import annotations

import argparse
import hashlib
import json
from collections import Counter, defaultdict
from pathlib import Path
from typing import Any

from c_source_probe import CSourceProbeError, function_span, sanitize_c
from check_frontend_tool_state_policy import (
    FrontendToolStatePolicyError,
    policy_decisions,
)
from postgresql_embedded_adapter import (
    EmbeddedAdapterError,
    load_adapter as load_embedded_adapter,
)
from generate_backend_state_plan import (
    byte_offset_for_line_column,
    declaration_terminator,
    safe_source,
)
from generate_tool_state_runtime import RUNTIME_KIND, relocation_bridge_name
from state_policy import catalog_candidates, load_json


def relocation_bridge(
    owner: dict[str, Any],
    owner_runtime: dict[str, Any],
    runtime_by_id: dict[str, Any],
) -> str:
    function = relocation_bridge_name(owner["id"])
    targets = [runtime_by_id[value] for value in owner_runtime["relocations"]]
    relocation_counts = owner_runtime.get("relocation_counts")
    if (
        not isinstance(relocation_counts, dict)
        or set(relocation_counts) != set(owner_runtime["relocations"])
        or not all(
            isinstance(value, int) and not isinstance(value, bool) and value > 0
            for value in relocation_counts.values()
        )
    ):
        raise FrontendToolStatePolicyError(
            f"runtime relocation counts are invalid for {owner['id']}"
        )
    lines = [
        "",
        "",
        "#ifdef POSTGAMMA_FRONTEND_TOOL",
        "void *",
        f"{function}(void)",
        "{",
        "\tstatic const PostgammaToolStateTemplate targets[] =",
        "\t{",
    ]
    for target in targets:
        expected_matches = relocation_counts[target["id"]]
        lines.append(
            f"\t\t{{{target['enum']}, &({target['name']}), "
            f"{expected_matches}}},"
        )
    lines.extend(
        [
            "\t};",
            "",
            "\treturn postgamma_tool_state_address_relocated(",
            f"\t\t{owner_runtime['enum']}, &({owner['name']}), targets,",
            "\t\tsizeof(targets) / sizeof(targets[0]));",
            "}",
            "#endif /* POSTGAMMA_FRONTEND_TOOL */",
        ]
    )
    return "\n".join(lines)


def compile_plan(
    catalog: dict[str, Any],
    policy: dict[str, Any],
    runtime: dict[str, Any],
    source_root: Path,
    embedded_adapter: dict[str, Any] | None = None,
) -> dict[str, Any]:
    candidates = catalog_candidates(catalog, FrontendToolStatePolicyError)
    decisions = policy_decisions(policy)
    if runtime.get("schema_version") != 1 or runtime.get("kind") != RUNTIME_KIND:
        raise FrontendToolStatePolicyError(
            f"runtime: expected schema_version 1 and kind {RUNTIME_KIND}"
        )
    slots = runtime.get("slots")
    if not isinstance(slots, list):
        raise FrontendToolStatePolicyError("runtime: slots must be an array")
    runtime_by_id = {entry["id"]: entry for entry in slots}
    tool_ids = {
        identifier
        for identifier in candidates
        if decisions[identifier]["owner"] == "tool"
    }
    if set(runtime_by_id) != tool_ids:
        raise FrontendToolStatePolicyError(
            "runtime slot inventory does not match tool ownership policy"
        )

    replacements_by_file: dict[
        str, dict[tuple[int, int], dict[str, Any]]
    ] = defaultdict(dict)
    use_counts: Counter[str] = Counter()
    initializer_classes: dict[tuple[str, int, int], set[bool]] = defaultdict(set)
    for index, use in enumerate(catalog.get("uses", [])):
        if not isinstance(use, dict):
            raise FrontendToolStatePolicyError(
                f"catalog: uses[{index}] must be an object"
            )
        if use.get("id") not in tool_ids or use.get("source_kind", "source") != "source":
            continue
        location = (use.get("path"), use.get("offset"), use.get("length"))
        if not isinstance(location[0], str) or not all(
            isinstance(value, int) for value in location[1:]
        ):
            raise FrontendToolStatePolicyError(
                f"catalog: uses[{index}] has an invalid source location"
            )
        initializer_classes[location].add(bool(use.get("in_static_initializer")))
    ambiguous = [
        location
        for location, classes in initializer_classes.items()
        if len(classes) > 1
    ]
    if ambiguous:
        raise FrontendToolStatePolicyError(
            "a frontend-tool source token is both an initializer and runtime use"
        )

    skipped_initializers = 0
    for use in catalog.get("uses", []):
        identifier = use.get("id")
        if identifier not in tool_ids or use.get("source_kind", "source") != "source":
            continue
        if use.get("in_static_initializer"):
            skipped_initializers += 1
            continue
        path = use["path"]
        offset = use["offset"]
        length = use["length"]
        candidate = candidates[identifier]
        slot = runtime_by_id[identifier]
        name = candidate["name"]
        if slot["relocations"]:
            replacement_text = (
                f"POSTGAMMA_TOOL_STATE_RELOCATED_VALUE({name}, "
                f"{relocation_bridge_name(identifier)})"
            )
        else:
            replacement_text = (
                f"POSTGAMMA_TOOL_STATE_VALUE({slot['enum']}, {name})"
            )
        use_kind = (
            "macro_body"
            if use.get("macro_kind") == "body"
            else use.get("use_kind", "read")
        )
        replacement = {
            "offset": offset,
            "length": length,
            "original": name,
            "replacement": replacement_text,
            "kind": "tool_state",
            "rule_id": identifier,
            "use_kind": use_kind,
            "line": use.get("line", 0),
            "column": use.get("column", 0),
        }
        key = (offset, length)
        existing = replacements_by_file[path].get(key)
        if existing is not None:
            if any(
                existing[field] != replacement[field]
                for field in ("original", "replacement", "rule_id")
            ):
                raise FrontendToolStatePolicyError(
                    f"inconsistent tool-state binding at {path}:{offset}+{length}"
                )
            if existing["use_kind"] != use_kind:
                existing["use_kind"] = "mixed"
            continue
        replacements_by_file[path][key] = replacement
        use_counts[use_kind] += 1

    for identifier in sorted(tool_ids):
        slot = runtime_by_id[identifier]
        if not slot["relocations"]:
            continue
        candidate = candidates[identifier]
        if candidate.get("definition_source_kind", "source") != "source":
            continue
        path = candidate["definition_path"]
        source = safe_source(source_root, path).read_bytes()
        name_offset = byte_offset_for_line_column(
            source, int(candidate["line"]), int(candidate["column"])
        )
        name = candidate["name"].encode("utf-8")
        if source[name_offset : name_offset + len(name)] != name:
            raise FrontendToolStatePolicyError(
                f"definition spelling changed for {identifier}"
            )
        insertion = declaration_terminator(source, name_offset + len(name))
        replacements_by_file[path][(insertion, 0)] = {
            "offset": insertion,
            "length": 0,
            "original": "",
            "replacement": relocation_bridge(candidate, slot, runtime_by_id),
            "kind": "tool_state_relocation_bridge",
            "rule_id": identifier,
            "use_kind": "injection",
            "line": candidate["line"],
            "column": candidate["column"],
        }

    phase_hook_count = 0
    for hook in (
        [] if embedded_adapter is None else embedded_adapter["frontend_tool_hooks"]
    ):
        path = hook["source_file"]
        source_path = safe_source(source_root, path)
        raw = source_path.read_text(encoding="utf-8")
        sanitized = sanitize_c(raw)
        scope_start, scope_end = function_span(
            sanitized,
            hook["enclosing_function"],
            hook["expected_definitions"],
        )
        anchor = hook["anchor"]
        matches: list[int] = []
        offset = scope_start
        while True:
            offset = raw.find(anchor, offset, scope_end)
            if offset < 0:
                break
            matches.append(offset)
            offset += len(anchor)
        if len(matches) != hook["expected_matches"]:
            raise FrontendToolStatePolicyError(
                f"frontend tool hook {hook['id']} expected "
                f"{hook['expected_matches']} match(es), found {len(matches)}"
            )
        for match in matches:
            byte_offset = len(raw[:match].encode("utf-8"))
            byte_length = len(anchor.encode("utf-8"))
            key = (byte_offset, byte_length)
            if key in replacements_by_file[path]:
                raise FrontendToolStatePolicyError(
                    f"frontend tool hook {hook['id']} overlaps a state replacement"
                )
            replacements_by_file[path][key] = {
                "offset": byte_offset,
                "length": byte_length,
                "original": anchor,
                "replacement": hook["replacement"],
                "kind": "frontend_tool_phase",
                "rule_id": hook["id"],
                "use_kind": "semantic_hook",
                "line": raw.count("\n", 0, match) + 1,
                "column": match - raw.rfind("\n", 0, match),
            }
            phase_hook_count += 1

    files: list[dict[str, Any]] = []
    replacement_count = 0
    for path in sorted(replacements_by_file):
        source = safe_source(source_root, path).read_bytes()
        replacements = sorted(
            replacements_by_file[path].values(),
            key=lambda item: (item["offset"], item["length"], item["rule_id"]),
        )
        previous_end = -1
        for replacement in replacements:
            start = replacement["offset"]
            end = start + replacement["length"]
            if start < previous_end:
                raise FrontendToolStatePolicyError(
                    f"overlapping tool-state replacements in {path} near {start}"
                )
            expected = replacement["original"].encode("utf-8")
            if source[start:end] != expected:
                raise FrontendToolStatePolicyError(
                    f"source spelling mismatch in {path} at {start}"
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
        "kind": "postgamma.tool-state-plan",
        "source_root": ".",
        "files": files,
        "summary": {
            "tool_candidate_count": len(tool_ids),
            "file_count": len(files),
            "replacement_count": replacement_count,
            "skipped_static_initializer_use_count": skipped_initializers,
            "relocation_bridge_count": sum(
                bool(runtime_by_id[identifier]["relocations"])
                for identifier in tool_ids
            ),
            "frontend_tool_phase_hook_count": phase_hook_count,
            "by_kind": dict(sorted(use_counts.items())),
        },
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--catalog", required=True, type=Path)
    parser.add_argument("--policy", required=True, type=Path)
    parser.add_argument("--runtime", required=True, type=Path)
    parser.add_argument("--source-root", required=True, type=Path)
    parser.add_argument("--embedded-adapter", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    try:
        plan = compile_plan(
            load_json(args.catalog, FrontendToolStatePolicyError),
            load_json(args.policy, FrontendToolStatePolicyError),
            load_json(args.runtime, FrontendToolStatePolicyError),
            args.source_root.resolve(),
            load_embedded_adapter(args.embedded_adapter.resolve()),
        )
    except (
        OSError,
        KeyError,
        TypeError,
        CSourceProbeError,
        EmbeddedAdapterError,
        FrontendToolStatePolicyError,
    ) as exc:
        parser.error(str(exc))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(
        json.dumps(plan, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    print(
        "frontend-tool state plan: "
        f"{plan['summary']['replacement_count']} replacement(s) in "
        f"{plan['summary']['file_count']} file(s)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
