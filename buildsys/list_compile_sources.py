#!/usr/bin/env python3
"""List deterministic source paths from a compilation database."""

from __future__ import annotations

import argparse
import json
import re
import shlex
import sys
from pathlib import Path

from merge_compile_db import source_root_kind


def string_array(document: dict[str, object], field: str) -> list[str]:
    value = document.get(field, [])
    if not isinstance(value, list) or not all(
        isinstance(item, str) and item for item in value
    ):
        raise ValueError(f"{field} must be an array of non-empty strings")
    return sorted(set(value))


def in_translation_unit_domain(
    relative: str, prefixes: list[str], files: list[str]
) -> bool:
    return relative in files or any(relative.startswith(prefix) for prefix in prefixes)


def select_candidates(
    domain_sources: list[tuple[str, Path, str]], manifest: dict[str, object]
) -> tuple[list[tuple[str, Path, str]], list[str], str]:
    """Select an AST over-approximation without weakening zero-match probes."""
    injections = manifest.get("injections", [])
    assumptions = manifest.get("assumptions", [])
    symbols = manifest.get("symbols", [])
    if not isinstance(injections, list) or not isinstance(assumptions, list):
        raise ValueError("injections and assumptions must be arrays")
    if not isinstance(symbols, list):
        raise ValueError("symbols must be an array")
    semantic_tokens = sorted(
        {str(item["name"]) for item in symbols if isinstance(item, dict)}
        | {
            str(item["anchor_callee"])
            for item in injections
            if isinstance(item, dict) and item.get("position") != "entry"
        }
        | {
            str(item["callee"])
            for item in assumptions
            if isinstance(item, dict)
        }
    )
    if not semantic_tokens:
        raise ValueError("candidate manifest contains no semantic selection tokens")
    if any(
        isinstance(item, dict) and item.get("expected_matches") == 0
        for item in assumptions
    ):
        return domain_sources, semantic_tokens, "full-domain-zero-match-assumption"
    pattern = re.compile(
        rb"\b(?:"
        + b"|".join(re.escape(name.encode("utf-8")) for name in semantic_tokens)
        + rb")\b"
    )
    forced_suffixes = {
        str(suffix)
        for item in injections
        if isinstance(item, dict)
        for suffix in item.get("source_file_suffixes", [])
    }
    selected = [
        item
        for item in domain_sources
        if pattern.search(item[1].read_bytes())
        or any(item[2].endswith(suffix) for suffix in forced_suffixes)
    ]
    return selected, semantic_tokens, "identifier-token-overapproximation"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--database", required=True, type=Path)
    parser.add_argument("--source-root", required=True, type=Path)
    parser.add_argument("--generated-root", type=Path)
    parser.add_argument("--candidate-manifest", type=Path)
    parser.add_argument("--domain-manifest", type=Path)
    parser.add_argument("--selection-report", type=Path)
    args = parser.parse_args()
    root = args.source_root.resolve()
    generated_root = args.generated_root.resolve() if args.generated_root else None
    records = json.loads(args.database.read_text(encoding="utf-8"))
    sources: set[tuple[str, Path, str]] = set()
    for index, record in enumerate(records):
        if not isinstance(record, dict) or not isinstance(record.get("file"), str):
            parser.error(f"compile database record {index} is invalid")
        source = Path(record["file"]).resolve()
        root_kind = source_root_kind(source, root, generated_root)
        if root_kind is None:
            continue
        logical_root = root if root_kind == "source" else generated_root
        assert logical_root is not None
        relative = source.relative_to(logical_root).as_posix()
        sources.add((root_kind, source, relative))
    all_sources = sorted(sources, key=lambda item: (item[0], item[2], str(item[1])))
    selected = all_sources
    domain_sources = all_sources
    domain_id: str | None = None
    semantic_tokens: list[str] = []
    selection_strategy = "all"
    if args.candidate_manifest and args.domain_manifest:
        parser.error("use either --candidate-manifest or --domain-manifest, not both")
    domain_manifest = args.candidate_manifest or args.domain_manifest
    if domain_manifest:
        manifest = json.loads(domain_manifest.read_text(encoding="utf-8"))
        if args.domain_manifest:
            value = manifest.get("id")
            if not isinstance(value, str) or not value:
                parser.error("domain manifest id must be a non-empty string")
            domain_id = value
        try:
            domain_prefixes = string_array(manifest, "translation_unit_prefixes")
            domain_files = string_array(manifest, "translation_unit_files")
        except ValueError as exc:
            parser.error(str(exc))
        if domain_prefixes or domain_files:
            domain_sources = [
                item
                for item in all_sources
                if in_translation_unit_domain(
                    item[2], domain_prefixes, domain_files
                )
            ]
            if not domain_sources:
                parser.error("translation-unit domain selected no source files")
        if args.candidate_manifest:
            try:
                selected, semantic_tokens, selection_strategy = select_candidates(
                    domain_sources, manifest
                )
            except ValueError as exc:
                parser.error(str(exc))
            if not selected:
                parser.error("candidate prefilter selected no translation units")
            print(
                f"candidate prefilter: {len(selected)}/{len(domain_sources)} domain "
                f"translation units ({len(all_sources)} captured total); "
                "semantic decisions remain AST-only",
                file=sys.stderr,
            )
        else:
            selected = domain_sources
            selection_strategy = "source-domain"

    if args.selection_report:
        report = {
            "schema_version": 1,
            "strategy": (
                selection_strategy
                if args.candidate_manifest or args.domain_manifest
                else "all"
            ),
            "semantic_tokens": semantic_tokens,
            "domain": domain_id,
            "total_translation_units": len(all_sources),
            "domain_translation_units": len(domain_sources),
            "selected_translation_units": len(selected),
            "files": [
                {"root": root_kind, "path": relative}
                for root_kind, _path, relative in selected
            ],
            "semantic_authority": "clang-ast",
        }
        args.selection_report.parent.mkdir(parents=True, exist_ok=True)
        args.selection_report.write_text(
            json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8"
        )

    print(" ".join(shlex.quote(str(path)) for _kind, path, _relative in selected))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
