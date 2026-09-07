#!/usr/bin/env python3
"""Record successful sanitizer execution for the embedded release boundary."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import Any

from check_embedded_lifecycle import input_identity, write_json


RUNTIME_STEMS = (
    "runtime-session-isolation",
    "runtime-path",
    "runtime-backend-execution",
    "runtime-instance",
    "runtime-postmaster-control",
)
EMBEDDED_STEMS = (
    "test-supervisor",
    "test-kernel-adapter",
    "test-data-directory-lock",
)


class SanitizerCheckError(RuntimeError):
    """A required sanitizer program or prior transport proof is missing."""


def load_json(path: Path) -> dict[str, Any]:
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise SanitizerCheckError(f"cannot read {path}: {exc}") from exc
    if not isinstance(document, dict):
        raise SanitizerCheckError(f"{path} is not a JSON object")
    return document


def collect_programs(
    directory: Path, stems: tuple[str, ...]
) -> dict[str, list[Path]]:
    programs = {
        "asan_ubsan_lsan": [directory / f"{stem}-asan" for stem in stems],
        "tsan": [directory / f"{stem}-tsan" for stem in stems],
    }
    for family, paths in programs.items():
        for path in paths:
            try:
                metadata = path.stat()
            except OSError as exc:
                raise SanitizerCheckError(
                    f"missing {family} program {path}"
                ) from exc
            if not path.is_file() or metadata.st_size == 0:
                raise SanitizerCheckError(
                    f"invalid {family} program {path}"
                )
    return programs


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--runtime-dir", required=True, type=Path)
    parser.add_argument("--embedded-dir", required=True, type=Path)
    parser.add_argument("--transport-evidence", required=True, type=Path)
    parser.add_argument("--input", action="append", default=[], type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()

    args.output.resolve().parent.mkdir(parents=True, exist_ok=True)
    args.output.resolve().unlink(missing_ok=True)
    try:
        runtime = collect_programs(
            args.runtime_dir.resolve(strict=True), RUNTIME_STEMS
        )
        embedded = collect_programs(
            args.embedded_dir.resolve(strict=True), EMBEDDED_STEMS
        )
        transport_path = args.transport_evidence.resolve(strict=True)
        transport = load_json(transport_path)
        if (
            transport.get("kind") != "postgamma.memory-transport-evidence"
            or transport.get("status") != "pass"
            or transport.get("metrics", {}).get("asan_lsan_ubsan") != "pass"
            or transport.get("metrics", {}).get("tsan") != "pass"
        ):
            raise SanitizerCheckError(
                "bounded memory transport sanitizer evidence is incomplete"
            )
        inputs = [path.resolve(strict=True) for path in args.input]
        program_paths = [
            path
            for families in (runtime, embedded)
            for paths in families.values()
            for path in paths
        ]
        document = {
            "schema_version": 1,
            "kind": "postgamma.release-sanitizer-evidence",
            "status": "pass",
            "postgresql_major": 19,
            "runtime": {
                "asan_ubsan_lsan_programs": len(runtime["asan_ubsan_lsan"]),
                "tsan_programs": len(runtime["tsan"]),
            },
            "embedded_boundary": {
                "asan_ubsan_lsan_programs": len(
                    embedded["asan_ubsan_lsan"]
                ),
                "tsan_programs": len(embedded["tsan"]),
            },
            "transport": {
                "asan_lsan_ubsan": "pass",
                "tsan": "pass",
            },
            "execution_contract": {
                "programs_built_and_executed_before_report": True,
                "halt_on_first_error": True,
            },
            "inputs": input_identity(
                [*inputs, transport_path, *program_paths]
            ),
        }
        write_json(args.output.resolve(), document)
    except (OSError, SanitizerCheckError) as exc:
        parser.error(str(exc))
    print(
        "embedded sanitizer evidence: pass "
        "(runtime, embedded boundary, bounded transport)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
