#!/usr/bin/env python3
"""Run the Clang inheritance inventory from declarative adapter facts."""

from __future__ import annotations

import argparse
import subprocess
from pathlib import Path

from postgresql_adapter import AdapterError, load_adapter


class InventoryError(ValueError):
    """The configured inheritance inventory cannot be executed safely."""


def command(
    tool: Path,
    source_root: Path,
    generated_root: Path,
    compile_database: Path,
    output: Path,
    adapter: dict[str, object],
) -> list[str]:
    inheritance = adapter["backend_inheritance"]
    if not isinstance(inheritance, dict):
        raise InventoryError("validated adapter has no backend inheritance object")
    source_file = inheritance["source_file"]
    contract_function = inheritance["contract_function"]
    extra_arguments = inheritance["extra_compile_arguments"]
    if not isinstance(source_file, str) or not isinstance(contract_function, str):
        raise InventoryError("validated adapter contains invalid inheritance facts")
    if not isinstance(extra_arguments, list):
        raise InventoryError("validated adapter contains invalid compile arguments")
    source = source_root / source_file
    if not source.is_file():
        raise InventoryError(f"inheritance source does not exist: {source}")
    if not compile_database.is_file():
        raise InventoryError(f"compile database does not exist: {compile_database}")
    return [
        str(tool),
        f"--source-root={source_root}",
        f"--generated-root={generated_root}",
        f"--contract-function={contract_function}",
        f"--output={output}",
        *(f"--extra-arg-before={argument}" for argument in extra_arguments),
        f"-p={compile_database.parent}",
        str(source),
    ]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--tool", required=True, type=Path)
    parser.add_argument("--source-root", required=True, type=Path)
    parser.add_argument("--generated-root", required=True, type=Path)
    parser.add_argument("--compile-database", required=True, type=Path)
    parser.add_argument("--adapter", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    try:
        adapter = load_adapter(args.adapter.resolve())
        invocation = command(
            args.tool.resolve(),
            args.source_root.resolve(),
            args.generated_root.resolve(),
            args.compile_database.resolve(),
            args.output.resolve(),
            adapter,
        )
        args.output.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run(invocation, check=True)
    except (AdapterError, InventoryError, OSError, subprocess.CalledProcessError) as exc:
        parser.error(str(exc))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
