#!/usr/bin/env python3
"""Install and transform configured-build artifacts from the AST oracle tree.

Flex and Bison embed source paths in their output, so regenerating the same
artifact below the disposable transformed source tree changes byte offsets.
The reference build is the byte-exact AST input.  Copy its generated artifact
as an immutable template, verify the plan hash, and then apply the semantic
edits in the generated build tree before any server object is compiled.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path

from apply_replacements import apply_bytes, safe_target
from reset_directory import is_strict_child


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def write_artifact(target: Path, content: bytes, mode: int) -> None:
    """Install changed bytes without inheriting stale reference-tree mtimes."""

    target.parent.mkdir(parents=True, exist_ok=True)
    if target.is_file() and target.read_bytes() == content:
        return
    target.write_bytes(content)
    target.chmod(mode)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--plan", required=True, type=Path)
    parser.add_argument("--template-root", required=True, type=Path)
    parser.add_argument("--tree", required=True, type=Path)
    parser.add_argument("--build-root", required=True, type=Path)
    args = parser.parse_args()

    plan_path = args.plan.resolve()
    template_root = args.template_root.resolve()
    tree = args.tree.resolve()
    build_root = args.build_root.resolve()
    if not is_strict_child(template_root, build_root) or not is_strict_child(
        tree, build_root
    ):
        parser.error("template and destination must be strict children of build root")
    for root, label in ((template_root, "template"), (tree, "destination")):
        if not (root / ".postgamma-configure.json").is_file():
            parser.error(f"{label} is not a postgamma-configured build tree: {root}")

    plan_bytes = plan_path.read_bytes()
    plan = json.loads(plan_bytes)
    if (
        not isinstance(plan, dict)
        or plan.get("schema_version") != 1
        or plan.get("mode") != "plan"
        or not isinstance(plan.get("files"), list)
    ):
        parser.error("unsupported generated-artifact replacement plan")

    total = 0
    output_hashes: dict[str, str] = {}
    companion_hashes: dict[str, str] = {}
    planned_paths = {str(entry["path"]) for entry in plan["files"]}
    try:
        for entry in sorted(plan["files"], key=lambda item: item["path"]):
            relative = str(entry["path"])
            template = safe_target(template_root, relative)
            target = safe_target(tree, relative)
            if not template.is_file():
                raise ValueError(f"generated template does not exist: {relative}")
            expected = str(entry["sha256"])
            actual = sha256(template)
            if actual != expected:
                raise ValueError(
                    f"generated template hash mismatch for {relative}: "
                    f"expected {expected}, got {actual}"
                )
            transformed, replacement_count = apply_bytes(template.read_bytes(), entry)
            write_artifact(target, transformed, template.stat().st_mode)
            total += replacement_count
            output_hashes[relative] = sha256(target)

            # Bison emits a .c/.h pair.  Once the transformed .c template is
            # installed, PostgreSQL's dependency rule intentionally only
            # touches the matching header; without the byte-exact companion
            # that creates an empty header.  Discover the pair structurally
            # instead of naming individual parsers in project policy.
            if template.suffix == ".c":
                companion_relative = str(Path(relative).with_suffix(".h"))
                companion_template = template.with_suffix(".h")
                if (
                    companion_relative not in planned_paths
                    and companion_template.is_file()
                ):
                    companion_target = safe_target(tree, companion_relative)
                    write_artifact(
                        companion_target,
                        companion_template.read_bytes(),
                        companion_template.stat().st_mode,
                    )
                    companion_hashes[companion_relative] = sha256(companion_target)
    except (KeyError, OSError, TypeError, ValueError) as exc:
        parser.error(str(exc))

    metadata = {
        "schema_version": 1,
        "kind": "postgamma.generated-artifact-replacements",
        "plan_sha256": hashlib.sha256(plan_bytes).hexdigest(),
        "replacement_count": total,
        "outputs": output_hashes,
        "companion_outputs": companion_hashes,
    }
    marker = tree / ".postgamma-generated-replacements.json"
    marker.write_text(json.dumps(metadata, indent=2, sort_keys=True) + "\n")
    print(f"installed {total} generated-artifact replacement(s) into {tree}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
