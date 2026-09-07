#!/usr/bin/env python3
"""Load and validate declarative PostgreSQL integration facts."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
from collections import Counter, defaultdict
from pathlib import Path, PurePosixPath
from typing import Any


ADAPTER_KIND = "postgamma.postgresql-adapter"
ADAPTER_SCHEMA_VERSION = 2
EDIT_MODES = frozenset(
    {"insert_after", "insert_before", "replace", "replace_between"}
)
SOURCE_ROOTS = frozenset({"project", "runtime"})
HOOK_POSITIONS = frozenset(
    {"before", "after", "after_enclosing_statement", "entry"}
)
SUPPORT_PROFILES = frozenset({"product", "validation"})
FACT_SOURCE_NAMES = frozenset({"backend_execution_catalog", "guc_catalog"})
VERSION_PATTERN = re.compile(
    r"^AC_INIT\(\[PostgreSQL\],\s*\[([0-9]+)[^]]*\]",
    re.MULTILINE,
)


class AdapterError(ValueError):
    """A PostgreSQL adapter or the source tree it describes is invalid."""


def _reject_unknown(value: dict[str, Any], allowed: set[str], label: str) -> None:
    unknown = sorted(set(value) - allowed)
    if unknown:
        raise AdapterError(f"{label} has unknown field(s): " + ", ".join(unknown))


def _unique_object(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    counts = Counter(key for key, _ in pairs)
    duplicates = sorted(key for key, count in counts.items() if count > 1)
    if duplicates:
        raise AdapterError("duplicate JSON key(s): " + ", ".join(duplicates))
    return dict(pairs)


def _relative_path(value: Any, label: str) -> str:
    if not isinstance(value, str) or not value:
        raise AdapterError(f"{label} must be a non-empty relative path")
    path = PurePosixPath(value)
    if path.is_absolute() or ".." in path.parts or "." in path.parts:
        raise AdapterError(f"{label} must not escape its declared root: {value!r}")
    return value


def _required_string(value: dict[str, Any], field: str, label: str) -> str:
    result = value.get(field)
    if not isinstance(result, str) or not result:
        raise AdapterError(f"{label}.{field} must be a non-empty string")
    return result


def _unique_ids(entries: list[Any], label: str) -> None:
    identifiers: list[str] = []
    for index, entry in enumerate(entries):
        if not isinstance(entry, dict):
            raise AdapterError(f"{label}[{index}] must be an object")
        identifiers.append(_required_string(entry, "id", f"{label}[{index}]"))
    duplicates = sorted(
        identifier
        for identifier, count in Counter(identifiers).items()
        if count > 1
    )
    if duplicates:
        raise AdapterError(f"{label} contains duplicate id(s): " + ", ".join(duplicates))


def _validate_edit(edit: dict[str, Any], label: str) -> None:
    _reject_unknown(
        edit,
        {
            "id",
            "path",
            "mode",
            "match",
            "start",
            "end",
            "content",
            "expected_matches",
            "reject_if_present",
        },
        label,
    )
    _relative_path(edit.get("path"), f"{label}.path")
    mode = edit.get("mode")
    if mode not in EDIT_MODES:
        raise AdapterError(
            f"{label}.mode must be one of {', '.join(sorted(EDIT_MODES))}"
        )
    if mode == "replace_between":
        _required_string(edit, "start", label)
        _required_string(edit, "end", label)
        if "match" in edit:
            raise AdapterError(f"{label}.match is not valid for replace_between")
    else:
        _required_string(edit, "match", label)
        if "start" in edit or "end" in edit:
            raise AdapterError(
                f"{label}.start and .end are valid only for replace_between"
            )
    content = edit.get("content")
    if not isinstance(content, str):
        raise AdapterError(f"{label}.content must be a string")
    expected = edit.get("expected_matches")
    if not isinstance(expected, int) or isinstance(expected, bool) or expected <= 0:
        raise AdapterError(f"{label}.expected_matches must be a positive integer")
    if mode == "replace_between" and expected != 1:
        raise AdapterError(
            f"{label}.expected_matches must be 1 for replace_between"
        )
    marker = edit.get("reject_if_present")
    if marker is not None and (not isinstance(marker, str) or not marker):
        raise AdapterError(f"{label}.reject_if_present must be a non-empty string")


def validate_adapter(document: dict[str, Any]) -> dict[str, Any]:
    _reject_unknown(
        document,
        {
            "schema_version",
            "kind",
            "id",
            "compatibility_scope",
            "supported_postgresql_majors",
            "fact_sources",
            "backend_inheritance",
            "runtime_hooks",
            "execution_model_assumptions",
            "generated_support",
            "test_adaptations",
        },
        "adapter",
    )
    if (
        document.get("schema_version") != ADAPTER_SCHEMA_VERSION
        or document.get("kind") != ADAPTER_KIND
    ):
        raise AdapterError(
            f"adapter must use schema_version {ADAPTER_SCHEMA_VERSION} and kind "
            f"{ADAPTER_KIND}"
        )
    _required_string(document, "id", "adapter")
    _required_string(document, "compatibility_scope", "adapter")

    inheritance = document.get("backend_inheritance")
    if not isinstance(inheritance, dict):
        raise AdapterError("adapter.backend_inheritance must be an object")
    _reject_unknown(
        inheritance,
        {"source_file", "contract_function", "extra_compile_arguments"},
        "adapter.backend_inheritance",
    )
    _relative_path(
        inheritance.get("source_file"),
        "adapter.backend_inheritance.source_file",
    )
    _required_string(
        inheritance,
        "contract_function",
        "adapter.backend_inheritance",
    )
    extra_arguments = inheritance.get("extra_compile_arguments")
    if (
        not isinstance(extra_arguments, list)
        or not extra_arguments
        or any(not isinstance(argument, str) or not argument for argument in extra_arguments)
        or len(extra_arguments) != len(set(extra_arguments))
    ):
        raise AdapterError(
            "adapter.backend_inheritance.extra_compile_arguments must contain "
            "unique non-empty strings"
        )

    majors = document.get("supported_postgresql_majors")
    if (
        not isinstance(majors, list)
        or not majors
        or any(not isinstance(major, int) or isinstance(major, bool) or major <= 0 for major in majors)
        or len(majors) != len(set(majors))
    ):
        raise AdapterError(
            "adapter.supported_postgresql_majors must contain unique positive integers"
        )

    fact_sources = document.get("fact_sources")
    if not isinstance(fact_sources, dict):
        raise AdapterError("adapter.fact_sources must be an object")
    _reject_unknown(fact_sources, set(FACT_SOURCE_NAMES), "adapter.fact_sources")
    missing_fact_sources = sorted(FACT_SOURCE_NAMES - set(fact_sources))
    if missing_fact_sources:
        raise AdapterError(
            "adapter.fact_sources is missing required source(s): "
            + ", ".join(missing_fact_sources)
        )
    for name in sorted(FACT_SOURCE_NAMES):
        source = fact_sources[name]
        label = f"adapter.fact_sources.{name}"
        if not isinstance(source, dict):
            raise AdapterError(f"{label} must be an object")
        _reject_unknown(source, {"path", "parser"}, label)
        _relative_path(source.get("path"), f"{label}.path")
        _required_string(source, "parser", label)

    hooks = document.get("runtime_hooks")
    if not isinstance(hooks, list):
        raise AdapterError("adapter.runtime_hooks must be an array")
    _unique_ids(hooks, "adapter.runtime_hooks")
    for index, hook in enumerate(hooks):
        label = f"adapter.runtime_hooks[{index}]"
        _reject_unknown(
            hook,
            {
                "id",
                "enclosing_function",
                "anchor_callee",
                "position",
                "code",
                "source_file_suffixes",
                "expected_matches",
            },
            label,
        )
        for field in ("enclosing_function", "anchor_callee", "code"):
            _required_string(hook, field, label)
        if hook.get("position") not in HOOK_POSITIONS:
            raise AdapterError(
                f"{label}.position must be one of "
                + ", ".join(sorted(HOOK_POSITIONS))
            )
        expected = hook.get("expected_matches")
        if not isinstance(expected, int) or isinstance(expected, bool) or expected <= 0:
            raise AdapterError(f"{label}.expected_matches must be a positive integer")
        suffixes = hook.get("source_file_suffixes")
        if suffixes is not None and (
            not isinstance(suffixes, list)
            or not suffixes
            or any(not isinstance(suffix, str) or not suffix for suffix in suffixes)
            or len(suffixes) != len(set(suffixes))
        ):
            raise AdapterError(
                f"{label}.source_file_suffixes must contain unique non-empty strings"
            )

    assumptions = document.get("execution_model_assumptions")
    if not isinstance(assumptions, list) or not assumptions:
        raise AdapterError(
            "adapter.execution_model_assumptions must be a non-empty array"
        )
    _unique_ids(assumptions, "adapter.execution_model_assumptions")
    for index, assumption in enumerate(assumptions):
        label = f"adapter.execution_model_assumptions[{index}]"
        _reject_unknown(
            assumption,
            {
                "id",
                "callee",
                "expected_matches",
                "allowed_source_file_suffixes",
                "rationale",
            },
            label,
        )
        _required_string(assumption, "callee", label)
        _required_string(assumption, "rationale", label)
        expected = assumption.get("expected_matches")
        if not isinstance(expected, int) or isinstance(expected, bool) or expected < 0:
            raise AdapterError(
                f"{label}.expected_matches must be a non-negative integer"
            )
        suffixes = assumption.get("allowed_source_file_suffixes")
        if (
            not isinstance(suffixes, list)
            or not suffixes
            or any(not isinstance(suffix, str) or not suffix for suffix in suffixes)
            or len(suffixes) != len(set(suffixes))
        ):
            raise AdapterError(
                f"{label}.allowed_source_file_suffixes must contain unique "
                "non-empty strings"
            )

    support = document.get("generated_support")
    if not isinstance(support, dict):
        raise AdapterError("adapter.generated_support must be an object")
    _reject_unknown(support, {"copies", "edits"}, "adapter.generated_support")
    copies = support.get("copies")
    edits = support.get("edits")
    if not isinstance(copies, list) or not copies:
        raise AdapterError("adapter.generated_support.copies must be a non-empty array")
    if not isinstance(edits, list) or not edits:
        raise AdapterError("adapter.generated_support.edits must be a non-empty array")
    _unique_ids(copies, "adapter.generated_support.copies")
    _unique_ids(edits, "adapter.generated_support.edits")

    copy_targets: list[str] = []
    for index, copy in enumerate(copies):
        label = f"adapter.generated_support.copies[{index}]"
        _reject_unknown(copy, {"id", "source_root", "source", "target"}, label)
        source_root = copy.get("source_root")
        if source_root not in SOURCE_ROOTS:
            raise AdapterError(
                f"{label}.source_root must be one of {', '.join(sorted(SOURCE_ROOTS))}"
            )
        _relative_path(copy.get("source"), f"{label}.source")
        copy_targets.append(_relative_path(copy.get("target"), f"{label}.target"))
    if len(copy_targets) != len(set(copy_targets)):
        raise AdapterError("adapter.generated_support.copies contains duplicate targets")

    for index, edit in enumerate(edits):
        _validate_edit(edit, f"adapter.generated_support.edits[{index}]")
        if edit["mode"] == "replace_between":
            raise AdapterError(
                "adapter.generated_support.edits may not use replace_between; "
                "the mode is restricted to validation-only test adaptations"
            )

    test_adaptations = document.get("test_adaptations")
    if not isinstance(test_adaptations, list):
        raise AdapterError("adapter.test_adaptations must be an array")
    _unique_ids(test_adaptations, "adapter.test_adaptations")
    for index, edit in enumerate(test_adaptations):
        _validate_edit(edit, f"adapter.test_adaptations[{index}]")

    all_edit_ids = [entry["id"] for entry in edits + test_adaptations]
    duplicate_edit_ids = sorted(
        identifier
        for identifier, count in Counter(all_edit_ids).items()
        if count > 1
    )
    if duplicate_edit_ids:
        raise AdapterError(
            "adapter product and test edits contain duplicate id(s): "
            + ", ".join(duplicate_edit_ids)
        )
    return document


def load_adapter(path: Path) -> dict[str, Any]:
    try:
        document = json.loads(
            path.read_text(encoding="utf-8"), object_pairs_hook=_unique_object
        )
    except (OSError, json.JSONDecodeError, AdapterError) as exc:
        raise AdapterError(f"cannot load PostgreSQL adapter {path}: {exc}") from exc
    if not isinstance(document, dict):
        raise AdapterError(f"{path}: top-level value must be an object")
    return validate_adapter(document)


def adapter_sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def postgresql_major(source: Path) -> int:
    configure_ac = source / "configure.ac"
    try:
        text = configure_ac.read_text(encoding="utf-8")
    except OSError as exc:
        raise AdapterError(f"cannot read {configure_ac}: {exc}") from exc
    matches = VERSION_PATTERN.findall(text)
    if len(matches) != 1:
        raise AdapterError(
            f"expected exactly one PostgreSQL AC_INIT version in {configure_ac}, found {len(matches)}"
        )
    return int(matches[0])


def validate_source_compatibility(
    adapter: dict[str, Any], source: Path
) -> int:
    major, declared = inspect_source_compatibility(adapter, source)
    supported = adapter["supported_postgresql_majors"]
    if not declared:
        raise AdapterError(
            f"adapter {adapter['id']} supports PostgreSQL major(s) "
            f"{', '.join(str(value) for value in supported)}, source is major {major}"
        )
    return major


def inspect_source_compatibility(
    adapter: dict[str, Any], source: Path
) -> tuple[int, bool]:
    """Return source major and whether it is in the product adapter contract."""
    major = postgresql_major(source)
    return major, major in adapter["supported_postgresql_majors"]


def fact_source(
    adapter: dict[str, Any], name: str
) -> dict[str, str]:
    """Return one validated upstream fact-source declaration."""
    if name not in FACT_SOURCE_NAMES:
        raise AdapterError(
            f"unknown PostgreSQL fact source {name!r}; expected one of "
            + ", ".join(sorted(FACT_SOURCE_NAMES))
        )
    source = adapter["fact_sources"][name]
    return {"path": source["path"], "parser": source["parser"]}


def generated_support_edits(
    adapter: dict[str, Any], profile: str = "validation"
) -> list[tuple[str, dict[str, Any]]]:
    """Return product edits and, for validation builds, test adaptations."""
    if profile not in SUPPORT_PROFILES:
        raise AdapterError(
            f"unknown generated-support profile {profile!r}; expected one of "
            + ", ".join(sorted(SUPPORT_PROFILES))
        )
    edits = [
        ("product", edit) for edit in adapter["generated_support"]["edits"]
    ]
    if profile == "validation":
        edits.extend(("test", edit) for edit in adapter["test_adaptations"])
    return edits


def inspect_edit_anchor(edit: dict[str, Any], text: str) -> dict[str, Any]:
    """Inspect one validated edit without mutating its source text."""
    marker = edit.get("reject_if_present")
    if marker is not None and marker in text:
        return {
            "status": "marker_present",
            "expected_matches": edit["expected_matches"],
            "actual_matches": None,
            "spans": [],
        }

    expected = edit["expected_matches"]
    if edit["mode"] != "replace_between":
        match = edit["match"]
        offsets: list[int] = []
        offset = text.find(match)
        while offset >= 0:
            offsets.append(offset)
            offset = text.find(match, offset + len(match))
        actual = len(offsets)
        if edit["mode"] == "insert_before":
            spans = [{"start": offset, "end": offset} for offset in offsets]
        elif edit["mode"] == "insert_after":
            spans = [
                {"start": offset + len(match), "end": offset + len(match)}
                for offset in offsets
            ]
        else:
            spans = [
                {"start": offset, "end": offset + len(match)} for offset in offsets
            ]
        return {
            "status": "ok" if actual == expected else "anchor_mismatch",
            "expected_matches": expected,
            "actual_matches": actual,
            "spans": spans,
        }

    start_matches = text.count(edit["start"])
    end_matches = text.count(edit["end"])
    start_offset = text.find(edit["start"])
    end_offset = text.find(edit["end"], start_offset + len(edit["start"]))
    ordered = start_offset >= 0 and end_offset >= 0
    actual = 1 if start_matches == 1 and end_matches == 1 and ordered else 0
    result = {
        "status": "ok" if actual == expected else "anchor_mismatch",
        "expected_matches": expected,
        "actual_matches": actual,
        "start_matches": start_matches,
        "end_matches": end_matches,
        "ordered": ordered,
        "spans": [],
    }
    if actual == 1:
        region_end = end_offset + len(edit["end"])
        region = text[start_offset:region_end].encode("utf-8")
        result.update(
            {
                "spans": [{"start": start_offset, "end": region_end}],
                "matched_region_length": len(region),
                "matched_region_sha256": hashlib.sha256(region).hexdigest(),
            }
        )
    return result


def _spans_overlap(left: dict[str, int], right: dict[str, int]) -> bool:
    """Return whether two source edits have order-dependent effects."""
    left_start, left_end = left["start"], left["end"]
    right_start, right_end = right["start"], right["end"]
    if left_start == left_end and right_start == right_end:
        return left_start == right_start
    if left_start == left_end:
        return right_start <= left_start <= right_end
    if right_start == right_end:
        return left_start <= right_start <= left_end
    return left_start < right_end and right_start < left_end


def mark_overlapping_edits(records: list[dict[str, Any]]) -> None:
    """Mark every edit whose source span conflicts with another edit."""
    by_path: dict[str, list[tuple[dict[str, Any], dict[str, int]]]] = defaultdict(list)
    for record in records:
        if record["status"] != "ok":
            continue
        for span in record.get("spans", []):
            by_path[record["path"]].append((record, span))
    conflicts: dict[str, set[str]] = defaultdict(set)
    for entries in by_path.values():
        entries.sort(
            key=lambda item: (
                item[1]["start"],
                item[1]["end"],
                item[0]["id"],
            )
        )
        for index, (left_record, left_span) in enumerate(entries):
            for right_record, right_span in entries[index + 1 :]:
                if right_span["start"] > left_span["end"]:
                    break
                if left_record["id"] == right_record["id"]:
                    continue
                if _spans_overlap(left_span, right_span):
                    conflicts[left_record["id"]].add(right_record["id"])
                    conflicts[right_record["id"]].add(left_record["id"])
    for record in records:
        if record["id"] in conflicts:
            record["status"] = "overlapping_edit"
            record["conflicts_with"] = sorted(conflicts[record["id"]])


def apply_edit(edit: dict[str, Any], text: str) -> str:
    """Apply one already-inspected edit to source text."""
    if edit["mode"] == "replace_between":
        start_offset = text.index(edit["start"])
        end_offset = text.index(edit["end"], start_offset + len(edit["start"]))
        end_offset += len(edit["end"])
        return text[:start_offset] + edit["content"] + text[end_offset:]
    match = edit["match"]
    if edit["mode"] == "insert_after":
        replacement = match + edit["content"]
    elif edit["mode"] == "insert_before":
        replacement = edit["content"] + match
    else:
        replacement = edit["content"]
    return text.replace(match, replacement)


def apply_non_overlapping_edits(
    edits: list[dict[str, Any]], text: str
) -> str:
    """Apply prevalidated edits against immutable source offsets."""
    replacements: list[tuple[int, int, str, str]] = []
    records: list[dict[str, Any]] = []
    for edit in edits:
        record = {"id": edit["id"], "path": "<memory>"}
        record.update(inspect_edit_anchor(edit, text))
        records.append(record)
        if record["status"] != "ok":
            raise AdapterError(f"{edit['id']}: {record['status']}")
        for span in record["spans"]:
            replacements.append(
                (span["start"], span["end"], edit["content"], edit["id"])
            )
    mark_overlapping_edits(records)
    failures = [record for record in records if record["status"] != "ok"]
    if failures:
        identifiers = ", ".join(record["id"] for record in failures)
        raise AdapterError(f"overlapping generated support edits: {identifiers}")
    for start, end, content, _identifier in sorted(
        replacements, key=lambda item: (item[0], item[1], item[3]), reverse=True
    ):
        text = text[:start] + content + text[end:]
    return text


def inspect_generated_support_anchors(
    adapter: dict[str, Any], source: Path, profile: str = "validation"
) -> list[dict[str, Any]]:
    """Return complete product and test edit compatibility diagnostics."""
    result: list[dict[str, Any]] = []
    for edit_class, edit in generated_support_edits(adapter, profile):
        target = source / edit["path"]
        record: dict[str, Any] = {
            "id": edit["id"],
            "class": edit_class,
            "path": edit["path"],
            "mode": edit["mode"],
            "severity": "error" if edit_class == "product" else "warning",
        }
        try:
            text = target.read_text(encoding="utf-8")
        except OSError as exc:
            record.update(
                {
                    "status": "unreadable",
                    "expected_matches": edit["expected_matches"],
                    "actual_matches": None,
                    "detail": str(exc),
                }
            )
        else:
            record.update(inspect_edit_anchor(edit, text))
        result.append(record)
    mark_overlapping_edits(result)
    return result


def format_anchor_failures(records: list[dict[str, Any]]) -> str:
    """Format all non-matching anchor diagnostics for a strict phase."""
    lines = ["generated support anchor validation failed:"]
    for record in records:
        if record["status"] == "ok":
            continue
        detail = ""
        if record["status"] == "unreadable":
            detail = f": {record['detail']}"
        elif record["status"] == "marker_present":
            detail = ": generated support marker is already present"
        elif record["status"] == "overlapping_edit":
            detail = ": conflicts with " + ", ".join(record["conflicts_with"])
        elif "start_matches" in record:
            detail = (
                f": start={record['start_matches']}, end={record['end_matches']}, "
                f"ordered={str(record['ordered']).lower()}"
            )
        else:
            detail = (
                f": expected {record['expected_matches']} exact match(es), "
                f"found {record['actual_matches']}"
            )
        lines.append(
            f"- {record['id']} [{record['class']}] in {record['path']}"
            f" ({record['status']}){detail}"
        )
    return "\n".join(lines)


def validate_generated_support_anchors(
    adapter: dict[str, Any], source: Path, profile: str = "validation"
) -> list[dict[str, Any]]:
    """Fail closed with all mismatches for the selected build profile."""
    result = inspect_generated_support_anchors(adapter, source, profile)
    failures = [record for record in result if record["status"] != "ok"]
    if failures:
        raise AdapterError(format_anchor_failures(failures))
    return result


def runtime_hooks_document(adapter: dict[str, Any]) -> dict[str, Any]:
    return {
        "schema_version": 1,
        "kind": "postgamma.runtime-hooks",
        "hooks": adapter["runtime_hooks"],
        "assumptions": adapter["execution_model_assumptions"],
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--adapter", required=True, type=Path)
    parser.add_argument("--source", type=Path)
    parser.add_argument("--profile", choices=sorted(SUPPORT_PROFILES), default="validation")
    parser.add_argument("--fact-source", choices=sorted(FACT_SOURCE_NAMES))
    parser.add_argument("--field", choices=("path", "parser"))
    args = parser.parse_args()
    try:
        adapter = load_adapter(args.adapter.resolve())
        if args.fact_source is not None:
            if args.field is None:
                parser.error("--field is required with --fact-source")
            print(fact_source(adapter, args.fact_source)[args.field])
            return 0
        if args.field is not None:
            parser.error("--field requires --fact-source")
        if args.source is None:
            parser.error("--source or --fact-source is required")
        major = validate_source_compatibility(adapter, args.source.resolve())
        anchors = validate_generated_support_anchors(
            adapter, args.source.resolve(), args.profile
        )
    except AdapterError as exc:
        parser.error(str(exc))
    print(
        f"PostgreSQL adapter: {adapter['id']} accepts major {major} "
        f"({len(adapter['runtime_hooks'])} runtime hooks, "
        f"{len(anchors)} {args.profile} generated-tree edits)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
