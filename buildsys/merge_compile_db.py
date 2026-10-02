#!/usr/bin/env python3
"""Merge atomic compiler fragments into a deterministic compilation database."""

from __future__ import annotations

import argparse
import json
import tempfile
from pathlib import Path


def inside(path: Path, root: Path) -> bool:
    try:
        path.resolve().relative_to(root.resolve())
        return True
    except ValueError:
        return False


def source_root_kind(
    path: Path, source_root: Path, generated_root: Path | None
) -> str | None:
    if inside(path, source_root):
        return "source"
    if generated_root is not None and inside(path, generated_root):
        return "generated_build"
    return None


def canonicalize_source_argument(
    arguments: list[str], directory: Path, source: Path
) -> list[str]:
    """Make the command's input spelling agree with its canonical file key.

    PostgreSQL configures platform-selected sources as build-tree symlinks such
    as pg_sema.c -> posix_sema.c.  ClangTool keys lookup by the canonical
    ``file`` member, but otherwise still parses the symlink spelling from
    ``arguments`` and reports every main-file location outside the source
    root.  Normalize the matching input token so those translation units
    cannot silently produce an empty AST inventory.
    """

    canonical = source.resolve()
    result = list(arguments)
    matches = 0
    for index, argument in enumerate(result):
        if index == 0 or not argument or argument.startswith("-"):
            continue
        candidate = Path(argument)
        if not candidate.is_absolute():
            candidate = directory / candidate
        try:
            matches_source = candidate.resolve() == canonical
        except OSError:
            matches_source = False
        if matches_source:
            result[index] = str(canonical)
            matches += 1
    if matches != 1:
        raise ValueError(
            f"expected one source argument for {canonical}, found {matches}"
        )
    return result


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--fragments", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--source-root", required=True, type=Path)
    parser.add_argument("--generated-root", type=Path)
    args = parser.parse_args()
    source_root = args.source_root.resolve()
    generated_root = args.generated_root.resolve() if args.generated_root else None

    records: dict[tuple[str, str, str, tuple[str, ...]], dict[str, object]] = {}
    for fragment in sorted(args.fragments.glob("*.json")):
        record = json.loads(fragment.read_text(encoding="utf-8"))
        source = Path(record["file"]).resolve()
        root_kind = source_root_kind(source, source_root, generated_root)
        if root_kind is None:
            continue
        directory = Path(record["directory"]).resolve()
        normalized = {
            "directory": str(directory),
            "file": str(source),
            "arguments": canonicalize_source_argument(
                list(record["arguments"]), directory, source
            ),
        }
        key = (
            root_kind,
            normalized["file"],
            normalized["directory"],
            tuple(normalized["arguments"]),
        )
        records[key] = normalized

    result = [records[key] for key in sorted(records)]
    if not result:
        parser.error(f"no source compile commands found beneath {source_root}")
    output = args.output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(
        mode="w", encoding="utf-8", dir=output.parent, prefix=".compile-db-", delete=False
    ) as handle:
        json.dump(result, handle, indent=2, sort_keys=True)
        handle.write("\n")
        temporary = Path(handle.name)
    temporary.replace(output)
    print(f"compile database: {len(result)} entries -> {output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
