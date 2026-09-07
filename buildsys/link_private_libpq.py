#!/usr/bin/env python3
"""Build a relocatable private libpq object and localize PostgreSQL symbols."""

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


RECEIPT_KIND = "postgamma.private-libpq-link"
SCHEMA_VERSION = 1
SYMBOL_PATTERN = re.compile(r"^[A-Za-z_][A-Za-z0-9_$.@]*$")
FORBIDDEN_UNDEFINED_PREFIXES = ("PQ", "pq", "pg_", "postgamma_")


class PrivateLibpqLinkError(RuntimeError):
    """The private libpq partial link or symbol localization failed."""


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
        raise PrivateLibpqLinkError(f"cannot execute {label}: {exc}") from exc
    if completed.returncode != 0:
        diagnostic = completed.stderr.strip() or completed.stdout.strip()
        raise PrivateLibpqLinkError(
            f"{label} failed with status {completed.returncode}: {diagnostic}"
        )
    return completed


def symbols(nm: str, path: Path, defined: bool) -> list[str]:
    command = [nm, "-P", "-g"]
    command.append("--defined-only" if defined else "--undefined-only")
    command.append(str(path))
    output = run(command, "nm").stdout
    result: list[str] = []
    for line in output.splitlines():
        fields = line.split()
        if not fields:
            continue
        name = fields[0]
        if name.endswith(":") or name == "_GLOBAL_OFFSET_TABLE_":
            continue
        if not SYMBOL_PATTERN.fullmatch(name):
            raise PrivateLibpqLinkError(f"nm returned an unsafe symbol: {name!r}")
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


def link(
    compiler: str,
    objcopy: str,
    nm: str,
    objects: list[Path],
    archives: list[Path],
    keep_prefixes: tuple[str, ...],
    required_symbols: tuple[str, ...],
    pull_symbols: tuple[str, ...],
    namespace_archive: Path | None,
    namespace_prefix: str | None,
    output: Path,
    receipt: Path,
) -> dict[str, Any]:
    inputs = [*objects, *archives]
    missing = [path for path in inputs if not path.is_file()]
    if missing:
        raise PrivateLibpqLinkError(
            "private libpq input(s) missing: "
            + ", ".join(str(path) for path in missing)
        )
    if not keep_prefixes or any(
        not prefix or not SYMBOL_PATTERN.fullmatch(prefix + "x")
        for prefix in keep_prefixes
    ):
        raise PrivateLibpqLinkError("keep prefixes are empty or unsafe")
    if any(not SYMBOL_PATTERN.fullmatch(name) for name in required_symbols):
        raise PrivateLibpqLinkError("required symbols contain an unsafe name")
    if any(not SYMBOL_PATTERN.fullmatch(name) for name in pull_symbols):
        raise PrivateLibpqLinkError("archive pull symbols contain an unsafe name")
    if len(pull_symbols) != len(set(pull_symbols)):
        raise PrivateLibpqLinkError("archive pull symbols contain duplicates")
    if (namespace_archive is None) != (namespace_prefix is None):
        raise PrivateLibpqLinkError(
            "namespace archive and namespace prefix must be specified together"
        )
    if namespace_archive is not None and not namespace_archive.is_file():
        raise PrivateLibpqLinkError(
            f"namespace archive is missing: {namespace_archive}"
        )
    if namespace_prefix is not None and (
        not namespace_prefix or
        not SYMBOL_PATTERN.fullmatch(namespace_prefix + "x")
    ):
        raise PrivateLibpqLinkError("namespace prefix is empty or unsafe")

    output.parent.mkdir(parents=True, exist_ok=True)
    receipt.parent.mkdir(parents=True, exist_ok=True)
    output.unlink(missing_ok=True)
    receipt.unlink(missing_ok=True)
    with tempfile.TemporaryDirectory(
        prefix=f".{output.name}.", dir=output.parent
    ) as directory:
        temporary_root = Path(directory)
        prelocal = temporary_root / "private-libpq-prelocal.o"
        namespaced = temporary_root / "private-libpq-namespaced.o"
        localized = temporary_root / output.name
        symbol_file = temporary_root / "localize-symbols.txt"
        namespace_file = temporary_root / "namespace-symbols.txt"
        link_command = [
            compiler,
            "-r",
            "-o",
            str(prelocal),
            *(str(path) for path in objects),
            *(f"-Wl,--undefined={name}" for name in pull_symbols),
            "-Wl,--start-group",
            *(str(path) for path in archives),
            "-Wl,--end-group",
        ]
        run(link_command, "private libpq partial link")
        definitions = symbols(nm, prelocal, defined=True)
        namespace_mapping: dict[str, str] = {}
        namespace_command: list[str] | None = None
        localization_input = prelocal
        if namespace_archive is not None and namespace_prefix is not None:
            namespace_candidates = set(
                symbols(nm, namespace_archive, defined=True)
            )
            namespace_mapping = {
                name: namespace_prefix + name
                for name in definitions
                if name in namespace_candidates
            }
            if not namespace_mapping:
                raise PrivateLibpqLinkError(
                    "namespace archive contributed no private libpq symbols"
                )
            targets = set(namespace_mapping.values())
            collisions = sorted(targets.intersection(definitions))
            if collisions:
                raise PrivateLibpqLinkError(
                    "private libpq namespace target collision(s): "
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
            run(namespace_command, "private libpq symbol namespacing")
            localization_input = namespaced
            definitions = symbols(nm, namespaced, defined=True)
        retained = sorted(
            name
            for name in definitions
            if any(name.startswith(prefix) for prefix in keep_prefixes)
        )
        missing_required = sorted(set(required_symbols) - set(retained))
        if missing_required:
            raise PrivateLibpqLinkError(
                "private libpq bridge symbol(s) missing: "
                + ", ".join(missing_required)
            )
        localized_symbols = sorted(set(definitions) - set(retained))
        if not localized_symbols:
            raise PrivateLibpqLinkError("private libpq has no symbols to localize")
        symbol_file.write_text("\n".join(localized_symbols) + "\n", encoding="utf-8")
        objcopy_command = [
            objcopy,
            f"--localize-symbols={symbol_file}",
            str(localization_input),
            str(localized),
        ]
        run(objcopy_command, "private libpq symbol localization")
        final_definitions = symbols(nm, localized, defined=True)
        if final_definitions != retained:
            raise PrivateLibpqLinkError(
                "private libpq retained an unexpected global definition"
            )
        undefined = symbols(nm, localized, defined=False)
        forbidden = sorted(
            name
            for name in undefined
            if name.startswith(FORBIDDEN_UNDEFINED_PREFIXES)
        )
        if forbidden:
            raise PrivateLibpqLinkError(
                "private libpq retained unresolved internal symbol(s): "
                + ", ".join(forbidden)
            )
        os.replace(localized, output)

    document: dict[str, Any] = {
        "schema_version": SCHEMA_VERSION,
        "kind": RECEIPT_KIND,
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
        "keep_prefixes": list(keep_prefixes),
        "required_symbols": list(required_symbols),
        "archive_pull_symbols": list(pull_symbols),
        "namespace_archive": (
            None if namespace_archive is None else str(namespace_archive)
        ),
        "namespace_prefix": namespace_prefix,
        "namespaced_symbols": namespace_mapping,
        "defined_before_localization": len(definitions),
        "localized_symbol_count": len(localized_symbols),
        "retained_symbols": retained,
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
    parser.add_argument("--object", action="append", default=[], type=Path)
    parser.add_argument("--archive", action="append", default=[], type=Path)
    parser.add_argument("--keep-prefix", action="append", default=[])
    parser.add_argument("--require-symbol", action="append", default=[])
    parser.add_argument("--pull-symbol", action="append", default=[])
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
            [path.resolve() for path in args.object],
            [path.resolve() for path in args.archive],
            tuple(args.keep_prefix),
            tuple(args.require_symbol),
            tuple(args.pull_symbol),
            None if args.namespace_archive is None else
            args.namespace_archive.resolve(),
            args.namespace_prefix,
            args.output.resolve(),
            args.receipt.resolve(),
        )
    except PrivateLibpqLinkError as exc:
        parser.error(str(exc))
    print(
        "private libpq link: "
        f"{document['localized_symbol_count']} symbol(s) localized, "
        f"{len(document['retained_symbols'])} bridge symbol(s) retained"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
