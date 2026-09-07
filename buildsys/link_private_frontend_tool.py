#!/usr/bin/env python3
"""Build one private relocatable PostgreSQL frontend-tool closure."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import subprocess
import tempfile
from pathlib import Path
from typing import Any


SCHEMA_VERSION = 1
RECEIPT_KIND = "postgamma.private-frontend-tool-link"
SYMBOL_PATTERN = re.compile(r"^[A-Za-z_][A-Za-z0-9_$.@]*$")
PRIVATE_PREFIXES = ("PQ", "pq", "pg_", "pgm_", "postgamma_")


class PrivateFrontendToolLinkError(RuntimeError):
    """The partial link or symbol-localization contract failed."""


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(command: list[str], label: str) -> subprocess.CompletedProcess[str]:
    try:
        completed = subprocess.run(
            command,
            check=False,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
    except OSError as exc:
        raise PrivateFrontendToolLinkError(
            f"cannot execute {label}: {exc}"
        ) from exc
    if completed.returncode != 0:
        diagnostic = completed.stderr.strip() or completed.stdout.strip()
        raise PrivateFrontendToolLinkError(
            f"{label} failed with status {completed.returncode}: {diagnostic}"
        )
    return completed


def symbols(nm: str, path: Path, defined: bool) -> list[str]:
    command = [nm, "-P", "-g"]
    command.append("--defined-only" if defined else "--undefined-only")
    command.append(str(path))
    result: list[str] = []
    for line in run(command, "nm").stdout.splitlines():
        fields = line.split()
        if not fields:
            continue
        name = fields[0]
        if name.endswith(":") or name == "_GLOBAL_OFFSET_TABLE_":
            continue
        if not SYMBOL_PATTERN.fullmatch(name):
            raise PrivateFrontendToolLinkError(
                f"nm returned an unsafe symbol: {name!r}"
            )
        result.append(name)
    return sorted(set(result))


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


def validate_symbol_list(values: tuple[str, ...], label: str) -> None:
    if not values or any(not SYMBOL_PATTERN.fullmatch(value) for value in values):
        raise PrivateFrontendToolLinkError(f"{label} is empty or unsafe")
    if len(values) != len(set(values)):
        raise PrivateFrontendToolLinkError(f"{label} contains duplicates")


def link(
    compiler: str,
    objcopy: str,
    nm: str,
    tool_id: str,
    objects: list[Path],
    archives: list[Path],
    kept_symbols: tuple[str, ...],
    allowed_undefined: tuple[str, ...],
    namespace_archive: Path | None,
    namespace_prefix: str | None,
    output: Path,
    receipt: Path,
) -> dict[str, Any]:
    inputs = [*objects, *archives]
    missing = [path for path in inputs if not path.is_file()]
    if missing:
        raise PrivateFrontendToolLinkError(
            "frontend-tool input(s) missing: "
            + ", ".join(str(path) for path in missing)
        )
    if not re.fullmatch(r"[a-z][a-z0-9-]*", tool_id):
        raise PrivateFrontendToolLinkError("tool id is empty or unsafe")
    validate_symbol_list(kept_symbols, "kept symbols")
    if any(not SYMBOL_PATTERN.fullmatch(value) for value in allowed_undefined):
        raise PrivateFrontendToolLinkError("allowed undefined symbols are unsafe")
    if (namespace_archive is None) != (namespace_prefix is None):
        raise PrivateFrontendToolLinkError(
            "namespace archive and namespace prefix must be specified together"
        )
    if namespace_archive is not None and not namespace_archive.is_file():
        raise PrivateFrontendToolLinkError(
            f"namespace archive is missing: {namespace_archive}"
        )
    if namespace_prefix is not None and (
        not namespace_prefix or
        not SYMBOL_PATTERN.fullmatch(namespace_prefix + "x")
    ):
        raise PrivateFrontendToolLinkError("namespace prefix is empty or unsafe")

    output.parent.mkdir(parents=True, exist_ok=True)
    receipt.parent.mkdir(parents=True, exist_ok=True)
    output.unlink(missing_ok=True)
    receipt.unlink(missing_ok=True)
    with tempfile.TemporaryDirectory(
        prefix=f".{output.name}.", dir=output.parent
    ) as directory:
        temporary_root = Path(directory)
        prelocal = temporary_root / f"{tool_id}-prelocal.o"
        namespaced = temporary_root / f"{tool_id}-namespaced.o"
        localized = temporary_root / output.name
        symbol_file = temporary_root / "localize-symbols.txt"
        namespace_file = temporary_root / "namespace-symbols.txt"
        link_command = [
            compiler,
            "-r",
            "-o",
            str(prelocal),
            *(str(path) for path in objects),
            "-Wl,--start-group",
            *(str(path) for path in archives),
            "-Wl,--end-group",
        ]
        run(link_command, f"{tool_id} partial link")
        definitions = symbols(nm, prelocal, defined=True)
        undefined_before_namespace = symbols(nm, prelocal, defined=False)
        namespace_mapping: dict[str, str] = {}
        namespace_command: list[str] | None = None
        localization_input = prelocal
        if namespace_archive is not None and namespace_prefix is not None:
            namespace_candidates = set(
                symbols(nm, namespace_archive, defined=True)
            )
            namespace_mapping = {
                name: namespace_prefix + name
                for name in undefined_before_namespace
                if name in namespace_candidates
            }
            if not namespace_mapping:
                raise PrivateFrontendToolLinkError(
                    "frontend tool has no references to namespace"
                )
            targets = set(namespace_mapping.values())
            collisions = sorted(targets.intersection(definitions))
            if collisions:
                raise PrivateFrontendToolLinkError(
                    "frontend-tool namespace target collision(s): "
                    + ", ".join(collisions)
                )
            namespace_file.write_text(
                "".join(
                    f"{source} {target}\n"
                    for source, target in sorted(namespace_mapping.items())
                ),
                encoding="utf-8",
            )
            namespace_command = [
                objcopy,
                f"--redefine-syms={namespace_file}",
                str(prelocal),
                str(namespaced),
            ]
            run(namespace_command, f"{tool_id} symbol namespacing")
            localization_input = namespaced
            definitions = symbols(nm, namespaced, defined=True)
        missing_kept = sorted(set(kept_symbols) - set(definitions))
        if missing_kept:
            raise PrivateFrontendToolLinkError(
                "frontend-tool entry symbol(s) missing: "
                + ", ".join(missing_kept)
            )
        localized_symbols = sorted(set(definitions) - set(kept_symbols))
        if not localized_symbols:
            raise PrivateFrontendToolLinkError(
                "frontend-tool closure has no private symbols to localize"
            )
        symbol_file.write_text(
            "\n".join(localized_symbols) + "\n", encoding="utf-8"
        )
        objcopy_command = [
            objcopy,
            f"--localize-symbols={symbol_file}",
            str(localization_input),
            str(localized),
        ]
        run(objcopy_command, f"{tool_id} symbol localization")
        final_definitions = symbols(nm, localized, defined=True)
        if final_definitions != sorted(kept_symbols):
            raise PrivateFrontendToolLinkError(
                "frontend-tool retained unexpected global definitions"
            )
        undefined = symbols(nm, localized, defined=False)
        forbidden = sorted(
            name
            for name in undefined
            if name.startswith(PRIVATE_PREFIXES)
            and name not in set(allowed_undefined)
            and name not in set(namespace_mapping.values())
        )
        if forbidden:
            raise PrivateFrontendToolLinkError(
                "frontend-tool retained unresolved internal symbol(s): "
                + ", ".join(forbidden)
            )
        os.replace(localized, output)

    document: dict[str, Any] = {
        "schema_version": SCHEMA_VERSION,
        "kind": RECEIPT_KIND,
        "tool_id": tool_id,
        "commands": [
            link_command,
            *([] if namespace_command is None else [namespace_command]),
            objcopy_command,
        ],
        "inputs": {
            str(path): sha256(path)
            for path in [
                *inputs,
                *([] if namespace_archive is None else [namespace_archive]),
            ]
        },
        "kept_symbols": sorted(kept_symbols),
        "allowed_undefined_symbols": sorted(allowed_undefined),
        "namespace_archive": (
            None if namespace_archive is None else str(namespace_archive)
        ),
        "namespace_prefix": namespace_prefix,
        "namespaced_symbols": namespace_mapping,
        "defined_before_localization": len(definitions),
        "localized_symbol_count": len(localized_symbols),
        "undefined_external_symbols": undefined,
        "output": str(output),
        "output_sha256": sha256(output),
    }
    write_json(receipt, document)
    return document


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", default="cc")
    parser.add_argument("--objcopy", default="objcopy")
    parser.add_argument("--nm", default="nm")
    parser.add_argument("--tool-id", required=True)
    parser.add_argument("--object", action="append", default=[], type=Path)
    parser.add_argument("--archive", action="append", default=[], type=Path)
    parser.add_argument("--keep-symbol", action="append", default=[])
    parser.add_argument("--allow-undefined-symbol", action="append", default=[])
    parser.add_argument("--namespace-archive", type=Path)
    parser.add_argument("--namespace-prefix")
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--receipt", required=True, type=Path)
    args = parser.parse_args()
    if not args.object or not args.archive:
        parser.error("at least one object and one archive are required")
    try:
        document = link(
            args.compiler,
            args.objcopy,
            args.nm,
            args.tool_id,
            [path.resolve() for path in args.object],
            [path.resolve() for path in args.archive],
            tuple(args.keep_symbol),
            tuple(args.allow_undefined_symbol),
            None if args.namespace_archive is None else
            args.namespace_archive.resolve(),
            args.namespace_prefix,
            args.output.resolve(),
            args.receipt.resolve(),
        )
    except PrivateFrontendToolLinkError as exc:
        parser.error(str(exc))
    print(
        f"private {document['tool_id']} link: "
        f"{document['localized_symbol_count']} symbol(s) localized, "
        f"{len(document['kept_symbols'])} entry symbol(s) retained"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
