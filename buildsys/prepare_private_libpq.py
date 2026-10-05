#!/usr/bin/env python3
"""Apply versioned provider hooks to the private PostgreSQL libpq sources."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import tempfile
from collections import defaultdict
from pathlib import Path
from typing import Any

from c_source_probe import CSourceProbeError, function_span, sanitize_c
from postgresql_embedded_adapter import load_adapter
from reset_directory import is_strict_child


REPORT_KIND = "postgamma.private-libpq-source"
SCHEMA_VERSION = 1


class PrivateLibpqSourceError(RuntimeError):
    """The declared private-libpq source seam drifted or is unsafe."""


def sha256_bytes(content: bytes) -> str:
    return hashlib.sha256(content).hexdigest()


def write_json(path: Path, document: dict[str, Any]) -> None:
    content = json.dumps(document, indent=2, sort_keys=True) + "\n"
    path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(
        mode="w",
        encoding="utf-8",
        dir=path.parent,
        prefix=f".{path.name}.",
        delete=False,
    ) as handle:
        handle.write(content)
        temporary = Path(handle.name)
    try:
        os.replace(temporary, path)
    finally:
        temporary.unlink(missing_ok=True)


def edit_for_hook(source: str, hook: dict[str, Any]) -> tuple[int, int, str]:
    sanitized = sanitize_c(source)
    start, end = function_span(
        sanitized, hook["enclosing_function"], hook["expected_definitions"]
    )
    if hook["kind"] == "entry_macro":
        arguments = ", ".join(hook["arguments"])
        return start + 1, start + 1, f"\n\t{hook['macro']}({arguments});"
    anchor = hook["anchor"]
    positions: list[int] = []
    offset = start
    while True:
        position = source.find(anchor, offset, end)
        if position < 0:
            break
        positions.append(position)
        offset = position + len(anchor)
    if len(positions) != 1:
        raise PrivateLibpqSourceError(
            f"hook {hook['id']} expected one anchor in "
            f"{hook['enclosing_function']}, found {len(positions)}"
        )
    position = positions[0]
    return position, position + len(anchor), hook["replacement"]


def transform_file(source: str, hooks: list[dict[str, Any]]) -> tuple[str, list[dict[str, Any]]]:
    edits: list[tuple[int, int, str, str]] = []
    facts: list[dict[str, Any]] = []
    for hook in hooks:
        start, end, replacement = edit_for_hook(source, hook)
        edits.append((start, end, replacement, hook["id"]))
        facts.append(
            {
                "id": hook["id"],
                "enclosing_function": hook["enclosing_function"],
                "kind": hook["kind"],
                "source_start": start,
                "source_end": end,
            }
        )
    ordered = sorted(edits, key=lambda item: (item[0], item[1]))
    for left, right in zip(ordered, ordered[1:]):
        if right[0] < left[1] or right[0] == left[0]:
            raise PrivateLibpqSourceError(
                f"hooks {left[3]} and {right[3]} overlap"
            )
    output = source
    for start, end, replacement, _identifier in reversed(ordered):
        output = output[:start] + replacement + output[end:]
    return output, sorted(facts, key=lambda item: item["id"])


def prepare(source_root: Path, adapter_path: Path, output: Path, report: Path) -> dict[str, Any]:
    source_root = source_root.resolve()
    output = output.resolve()
    boundary = output.parent.resolve()
    if not is_strict_child(output, boundary):
        raise PrivateLibpqSourceError("output must be a strict child of its parent")
    adapter = load_adapter(adapter_path.resolve())
    hooks_by_file: dict[str, list[dict[str, Any]]] = defaultdict(list)
    for hook in adapter["libpq_memory_hooks"]:
        hooks_by_file[hook["source_file"]].append(hook)
    temporary = Path(
        tempfile.mkdtemp(prefix=f".{output.name}.", dir=boundary)
    )
    files: list[dict[str, Any]] = []
    try:
        for relative, hooks in sorted(hooks_by_file.items()):
            source_path = source_root / relative
            content = source_path.read_bytes()
            text = content.decode("utf-8")
            transformed, facts = transform_file(text, hooks)
            transformed_bytes = transformed.encode("utf-8")
            target = temporary / Path(relative).name
            target.write_bytes(transformed_bytes)
            files.append(
                {
                    "source_file": relative,
                    "output_file": target.name,
                    "source_sha256": sha256_bytes(content),
                    "output_sha256": sha256_bytes(transformed_bytes),
                    "hooks": facts,
                }
            )
        if output.exists():
            shutil.rmtree(output)
        os.replace(temporary, output)
    finally:
        if temporary.exists():
            shutil.rmtree(temporary)
    document = {
        "schema_version": SCHEMA_VERSION,
        "kind": REPORT_KIND,
        "adapter_id": adapter["id"],
        "source_root": str(source_root),
        "output_root": str(output),
        "hook_count": sum(len(file["hooks"]) for file in files),
        "files": files,
    }
    write_json(report.resolve(), document)
    return document


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", required=True, type=Path)
    parser.add_argument("--adapter", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--report", required=True, type=Path)
    args = parser.parse_args()
    try:
        document = prepare(args.source, args.adapter, args.output, args.report)
    except (PrivateLibpqSourceError, CSourceProbeError, OSError, ValueError) as exc:
        parser.error(str(exc))
    print(
        f"private libpq source: {document['hook_count']} provider hook(s) "
        f"in {len(document['files'])} file(s)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
