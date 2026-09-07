#!/usr/bin/env python3
"""Build a deterministic, symbol-isolated PostGamma static library."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shlex
import shutil
import subprocess
import tempfile
from pathlib import Path
from typing import Sequence


RECEIPT_KIND = "postgamma.static-library-build"
PUBLIC_SYMBOL = re.compile(r"^pgm_[A-Za-z0-9_]+$")
CONSUMER_OPTION = re.compile(r"^(?:-pthread|-l[A-Za-z0-9_+.-]+)$")
GROUP_OPTIONS = {
    "--start-group",
    "--end-group",
    "--whole-archive",
    "--no-whole-archive",
    "-Wl,--start-group",
    "-Wl,--end-group",
    "-Wl,--whole-archive",
    "-Wl,--no-whole-archive",
}


class StaticLibraryError(RuntimeError):
    """The captured kernel closure cannot form a supported static library."""


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def run(arguments: Sequence[str], *, cwd: Path | None = None) -> str:
    try:
        completed = subprocess.run(
            list(arguments),
            cwd=cwd,
            check=False,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
    except OSError as error:
        raise StaticLibraryError(
            f"cannot execute {arguments[0]}: {error}"
        ) from error
    if completed.returncode != 0:
        detail = completed.stderr.strip() or completed.stdout.strip()
        raise StaticLibraryError(
            f"{' '.join(arguments)} failed with status "
            f"{completed.returncode}: {detail}"
        )
    return completed.stdout


def load_response(path: Path) -> list[str]:
    try:
        arguments = shlex.split(path.read_text(encoding="utf-8"), posix=True)
    except (OSError, ValueError) as error:
        raise StaticLibraryError(f"cannot parse response file {path}: {error}") from error
    if not arguments:
        raise StaticLibraryError("kernel response file is empty")
    return arguments


def classify_response(
    arguments: Sequence[str], work_directory: Path
) -> tuple[list[str], list[str], list[Path]]:
    """Return partial-link arguments, consumer options, and concrete inputs."""

    partial: list[str] = []
    consumer: list[str] = []
    inputs: list[Path] = []
    for argument in arguments:
        if argument in GROUP_OPTIONS:
            partial.append(argument)
            continue
        if CONSUMER_OPTION.fullmatch(argument):
            if argument not in consumer:
                consumer.append(argument)
            continue
        if argument.startswith("-L"):
            continue
        if argument.startswith("-"):
            continue
        candidate = Path(argument)
        if candidate.suffix not in {".a", ".o"}:
            continue
        resolved = candidate if candidate.is_absolute() else work_directory / candidate
        try:
            resolved = resolved.resolve(strict=True)
        except FileNotFoundError as error:
            raise StaticLibraryError(
                f"kernel response input does not exist: {argument}"
            ) from error
        if not resolved.is_file():
            raise StaticLibraryError(
                f"kernel response input is not a file: {argument}"
            )
        partial.append(str(resolved))
        inputs.append(resolved)
    if not inputs:
        raise StaticLibraryError("kernel response contains no object or archive inputs")
    return partial, consumer, inputs


def global_definitions(nm: str, path: Path) -> set[str]:
    output = run(
        (
            nm,
            "--defined-only",
            "--extern-only",
            "--format=posix",
            str(path),
        )
    )
    symbols: set[str] = set()
    for line in output.splitlines():
        fields = line.split()
        if fields and not fields[0].endswith(":"):
            symbols.add(fields[0])
    return symbols


def closure_digest(paths: Sequence[Path]) -> str:
    digest = hashlib.sha256()
    for path in paths:
        encoded = str(path).encode("utf-8")
        digest.update(len(encoded).to_bytes(8, "big"))
        digest.update(encoded)
        digest.update(bytes.fromhex(sha256(path)))
    return digest.hexdigest()


def build_once(
    *,
    compiler: str,
    archiver: str,
    nm: str,
    objcopy: str,
    work_directory: Path,
    direct_inputs: Sequence[Path],
    partial_arguments: Sequence[str],
    destination: Path,
) -> tuple[set[str], int]:
    destination.parent.mkdir(parents=True, exist_ok=True)
    combined = destination.parent / "postgamma-combined.o"
    localized = destination.parent / "postgamma-localized.o"
    symbols_file = destination.parent / "localize-symbols.txt"
    run(
        (
            compiler,
            "-r",
            "-Wl,--build-id=none",
            *(str(path) for path in direct_inputs),
            *partial_arguments,
            "-o",
            str(combined),
        ),
        cwd=work_directory,
    )
    definitions = global_definitions(nm, combined)
    public = {symbol for symbol in definitions if PUBLIC_SYMBOL.fullmatch(symbol)}
    internal = definitions - public
    if not public:
        raise StaticLibraryError("partial link defines no pgm_* public symbols")
    symbols_file.write_text(
        "".join(symbol + "\n" for symbol in sorted(internal)), encoding="utf-8"
    )
    shutil.copy2(combined, localized)
    if internal:
        run((objcopy, f"--localize-symbols={symbols_file}", str(localized)))
    remaining = global_definitions(nm, localized)
    if remaining != public:
        raise StaticLibraryError(
            "symbol localization left a non-public global definition"
        )
    run((archiver, "crsD", str(destination), str(localized)))
    if not destination.is_file():
        raise StaticLibraryError("archiver reported success without an output library")
    return public, len(internal)


def pkg_config_content(version: str, consumer_options: Sequence[str]) -> str:
    private = " ".join(consumer_options)
    return (
        "prefix=${pcfiledir}/../..\n"
        "exec_prefix=${prefix}\n"
        "libdir=${prefix}/lib\n"
        "includedir=${prefix}/include\n"
        "\n"
        "Name: PostGamma\n"
        "Description: In-process PostgreSQL-compatible embedded database\n"
        "URL: https://postgamma.com\n"
        f"Version: {version}\n"
        "Libs: ${libdir}/libpostgamma.a\n"
        f"Libs.private: {private}\n"
        "Cflags: -I${includedir}\n"
    )


def atomic_write(path: Path, content: bytes, mode: int = 0o644) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary_name = tempfile.mkstemp(
        dir=path.parent, prefix=f".{path.name}."
    )
    temporary = Path(temporary_name)
    try:
        with os.fdopen(descriptor, "wb") as stream:
            stream.write(content)
        temporary.chmod(mode)
        os.replace(temporary, path)
    finally:
        temporary.unlink(missing_ok=True)


def build(args: argparse.Namespace) -> dict[str, object]:
    work_directory = args.work_directory.resolve(strict=True)
    response_file = args.response_file.resolve(strict=True)
    direct_inputs = [path.resolve(strict=True) for path in args.input]
    partial, consumer_options, closure_inputs = classify_response(
        load_response(response_file), work_directory
    )
    for option in args.consumer_option:
        if not CONSUMER_OPTION.fullmatch(option):
            raise StaticLibraryError(f"unsupported consumer link option: {option}")
        if option not in consumer_options:
            consumer_options.append(option)
    output = args.output.resolve()
    public_headers = [path.resolve(strict=True) for path in args.public_header]
    if len({path.name for path in public_headers}) != len(public_headers):
        raise StaticLibraryError("public static SDK header names must be unique")
    sdk_root = output.parent.parent
    header_outputs = [
        sdk_root / "include" / "postgamma" / path.name for path in public_headers
    ]
    link_options = args.link_options.resolve()
    pkg_config = args.pkg_config.resolve()
    receipt = args.receipt.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".postgamma-static-", dir=output.parent) as root:
        temporary = Path(root)
        first = temporary / "first/libpostgamma.a"
        second = temporary / "second/libpostgamma.a"
        public, localized_count = build_once(
            compiler=args.compiler,
            archiver=args.archiver,
            nm=args.nm,
            objcopy=args.objcopy,
            work_directory=work_directory,
            direct_inputs=direct_inputs,
            partial_arguments=partial,
            destination=first,
        )
        repeated_public, repeated_localized_count = build_once(
            compiler=args.compiler,
            archiver=args.archiver,
            nm=args.nm,
            objcopy=args.objcopy,
            work_directory=work_directory,
            direct_inputs=direct_inputs,
            partial_arguments=partial,
            destination=second,
        )
        if (
            sha256(first) != sha256(second)
            or public != repeated_public
            or localized_count != repeated_localized_count
        ):
            raise StaticLibraryError("repeated static-library builds are not deterministic")
        library_content = first.read_bytes()

    options_content = "".join(shlex.quote(value) + "\n" for value in consumer_options)
    pkg_content = pkg_config_content(args.version, consumer_options)
    document: dict[str, object] = {
        "schema_version": 1,
        "kind": RECEIPT_KIND,
        "status": "pass",
        "format": "deterministic-archive",
        "linkage": "static",
        "version": args.version,
        "output": str(output),
        "output_sha256": hashlib.sha256(library_content).hexdigest(),
        "output_size": len(library_content),
        "public_symbols": sorted(public),
        "public_symbol_count": len(public),
        "localized_internal_symbol_count": localized_count,
        "consumer_link_options": consumer_options,
        "closure": {
            "response_file": str(response_file),
            "response_sha256": sha256(response_file),
            "input_count": len(closure_inputs) + len(direct_inputs),
            "input_digest": closure_digest([*direct_inputs, *closure_inputs]),
        },
        "determinism": {
            "build_count": 2,
            "identical": True,
        },
        "headers": [
            {
                "path": str(destination.relative_to(sdk_root)),
                "sha256": sha256(source),
            }
            for source, destination in zip(
                public_headers, header_outputs, strict=True
            )
        ],
    }
    atomic_write(output, library_content)
    for source, destination in zip(public_headers, header_outputs, strict=True):
        atomic_write(destination, source.read_bytes())
    atomic_write(link_options, options_content.encode("utf-8"))
    atomic_write(pkg_config, pkg_content.encode("utf-8"))
    atomic_write(
        receipt,
        (json.dumps(document, indent=2, sort_keys=True) + "\n").encode("utf-8"),
    )
    return document


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", default="cc")
    parser.add_argument("--archiver", default="ar")
    parser.add_argument("--nm", default="nm")
    parser.add_argument("--objcopy", default="objcopy")
    parser.add_argument("--work-directory", required=True, type=Path)
    parser.add_argument("--input", action="append", default=[], type=Path)
    parser.add_argument("--public-header", action="append", default=[], type=Path)
    parser.add_argument("--response-file", required=True, type=Path)
    parser.add_argument("--consumer-option", action="append", default=[])
    parser.add_argument("--version", required=True)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--link-options", required=True, type=Path)
    parser.add_argument("--pkg-config", required=True, type=Path)
    parser.add_argument("--receipt", required=True, type=Path)
    args = parser.parse_args()
    if not args.input:
        parser.error("at least one --input is required")
    if not args.public_header:
        parser.error("at least one --public-header is required")
    try:
        document = build(args)
    except (OSError, StaticLibraryError) as error:
        parser.error(str(error))
    print(
        "PostGamma static library: pass "
        f"({document['public_symbol_count']} public symbols, deterministic archive)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
