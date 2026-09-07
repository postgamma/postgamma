#!/usr/bin/env python3
"""Generate C data for the PostgreSQL embedded GUC policy."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path

from postgresql_embedded_adapter import load_adapter


def emit_setting_rows(settings: list[dict[str, str]]) -> list[str]:
    lines: list[str] = []
    for setting in settings:
        condition = setting["compile_condition"]
        if condition:
            lines.append(f"#ifdef {condition}")
        name = json.dumps(setting["name"], ensure_ascii=True)
        value = json.dumps(setting["value"], ensure_ascii=True)
        lines.append(f"\t{{{name}, {value}}},")
        if condition:
            lines.append(f"#endif /* {condition} */")
    return lines


def emit_policy(adapter: dict[str, object]) -> str:
    policy = adapter["guc_policy"]
    assert isinstance(policy, dict)
    defaults = policy["defaults"]
    safety = policy["safety"]
    assert isinstance(defaults, list)
    assert isinstance(safety, list)
    lines = [
        "/* Generated from the PostgreSQL-major-specific embedded adapter. */",
        "static const PostgammaEmbeddedSetting PostgammaEmbeddedDefaults[] =",
        "{",
        *emit_setting_rows(defaults),
        "};",
        "",
        "",
        "static const PostgammaEmbeddedSetting PostgammaEmbeddedSafetySettings[] =",
        "{",
        *emit_setting_rows(safety),
        "};",
        "",
    ]
    return "\n".join(lines)


def write_if_changed(path: Path, content: str) -> None:
    encoded = content.encode("utf-8")
    if path.is_file() and path.read_bytes() == encoded:
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_bytes(encoded)
    temporary.replace(path)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--adapter", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--stamp", required=True, type=Path)
    args = parser.parse_args()

    adapter = load_adapter(args.adapter)
    content = emit_policy(adapter)
    write_if_changed(args.output, content)
    policy = adapter["guc_policy"]
    assert isinstance(policy, dict)
    stamp = {
        "schema_version": 1,
        "kind": "postgamma.generated-embedded-guc-policy",
        "adapter_id": adapter["id"],
        "default_count": len(policy["defaults"]),
        "safety_count": len(policy["safety"]),
        "output": args.output.name,
        "sha256": hashlib.sha256(content.encode("utf-8")).hexdigest(),
    }
    write_if_changed(
        args.stamp,
        json.dumps(stamp, indent=2, sort_keys=True) + "\n",
    )
    args.stamp.touch()
    print(
        "generated embedded GUC policy "
        f"({stamp['default_count']} defaults, {stamp['safety_count']} safety settings)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
