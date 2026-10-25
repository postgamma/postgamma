#!/usr/bin/env python3
"""Install explicit template providers in an isolated transformed source tree.

This does not activate preparation. The resulting compilation and link closure
must pass qualify_backend_state_preparation before its registry can be used.
"""

from __future__ import annotations

import argparse
from collections import defaultdict
import hashlib
import json
from pathlib import Path

from generate_backend_state_plan import safe_source


def provider_name(identifier: str) -> str:
    return "postgamma_backend_state_template_" + hashlib.sha256(identifier.encode()).hexdigest()[:16]


def provider_ids(plan: dict) -> set[str]:
    slots = {slot["identifier"]: slot for slot in plan["slots"]}
    result = set()
    for slot in slots.values():
        if slot["preparation_candidate"]:
            if slot["initialization"] == "copy_template":
                result.add(slot["identifier"])
            result.update(slot["relocations"])
    return result


def install(plan: dict, source: Path, generated: Path) -> dict:
    if plan.get("kind") != "postgamma.backend-state-preparation-plan":
        raise ValueError("expected preparation candidate plan")
    needed = provider_ids(plan)
    files = defaultdict(list)
    for slot in plan["slots"]:
        if slot["identifier"] in needed:
            if slot["identifier"].startswith("function_static:"):
                raise ValueError("function-local templates cannot have file-scope providers")
            files[slot["definition"]].append(slot)
    receipt = {"schema_version": 1, "kind": "postgamma.backend-state-template-providers",
               "files": {}, "providers": {}}
    for name, slots in sorted(files.items()):
        root, path = ((generated, name[len("@generated/"):]) if name.startswith("@generated/")
                      else (source, name))
        file = safe_source(root.resolve(), path)
        original = file.read_bytes()
        marker = b"/* PostGamma explicit preparation template providers. */"
        if marker in original:
            raise ValueError(f"template providers already installed: {name}")
        lines = ["", marker.decode(), "#ifndef FRONTEND"]
        for slot in slots:
            function = provider_name(slot["identifier"])
            lines.extend([f"const volatile void *{function}(void);", "const volatile void *",
                          f"{function}(void)", "{", f"\treturn &({slot['name']});", "}"])
            receipt["providers"][slot["identifier"]] = function
        lines.extend(["#endif /* FRONTEND */", ""])
        data = original + "\n".join(lines).encode()
        file.write_bytes(data)
        receipt["files"][name] = {
            "before": hashlib.sha256(original).hexdigest(),
            "after": hashlib.sha256(data).hexdigest()}
    return receipt


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("plan", "source-root", "generated-root", "output"):
        parser.add_argument("--" + name, type=Path, required=True)
    args = parser.parse_args()
    try:
        receipt = install(json.loads(args.plan.read_text()), args.source_root, args.generated_root)
    except (ValueError, OSError, KeyError) as exc:
        parser.error(str(exc))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(receipt, indent=2, sort_keys=True) + "\n")
    print(f"preparation templates: {len(receipt['providers'])} explicit providers")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
