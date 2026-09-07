#!/usr/bin/env python3
"""Generate two complete transformation products and compare every output byte."""

from __future__ import annotations

import argparse
import hashlib
import os
import shlex
import shutil
import stat
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path


def run(arguments: list[str], *, cwd: Path) -> subprocess.CompletedProcess[str]:
    result = subprocess.run(
        arguments,
        cwd=cwd,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    if result.returncode:
        raise RuntimeError(
            f"command failed ({result.returncode}): {shlex.join(arguments)}\n"
            f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        )
    return result


def require_build_child(path: Path, project_root: Path) -> None:
    build = (project_root / "build").resolve()
    try:
        path.relative_to(build)
    except ValueError as exc:
        raise ValueError(f"determinism path must be below {build}: {path}") from exc
    if path == build:
        raise ValueError("refusing to use the complete build directory")


def generate_tree(
    root: Path,
    source: Path,
    upstream_manifest: Path,
    adapter: Path,
    plan: Path,
    destination: Path,
    runtime_dir: Path,
    support_profile: str,
) -> None:
    run(
        [
            sys.executable,
            str(root / "buildsys/materialize.py"),
            "--repository",
            str(source),
            "--manifest",
            str(upstream_manifest),
            "--destination",
            str(destination),
            "--build-root",
            str(destination.parent),
        ],
        cwd=root,
    )
    run(
        [
            sys.executable,
            str(root / "buildsys/apply_replacements.py"),
            "--plan",
            str(plan),
            "--tree",
            str(destination),
        ],
        cwd=root,
    )
    run(
        [
            sys.executable,
            str(root / "buildsys/install_generated_support.py"),
            "--project-root",
            str(root),
            "--runtime-dir",
            str(runtime_dir),
            "--tree",
            str(destination),
            "--adapter",
            str(adapter),
            "--profile",
            support_profile,
        ],
        cwd=root,
    )


def tree_manifest(tree: Path) -> list[tuple[str, int, str]]:
    records: list[tuple[str, int, str]] = []
    for path in sorted(tree.rglob("*")):
        relative = path.relative_to(tree).as_posix()
        mode = stat.S_IMODE(path.lstat().st_mode)
        if path.is_symlink():
            digest = "symlink:" + os.readlink(path)
        elif path.is_file():
            digest = hashlib.sha256(path.read_bytes()).hexdigest()
        elif path.is_dir():
            digest = "directory"
        else:
            raise RuntimeError(f"unsupported generated entry: {relative}")
        records.append((relative, mode, digest))
    return records


def manifest_digest(records: list[tuple[str, int, str]]) -> str:
    digest = hashlib.sha256()
    for path, mode, content_hash in records:
        digest.update(f"{path}\0{mode:o}\0{content_hash}\n".encode("utf-8"))
    return digest.hexdigest()


def require_equal_trees(label: str, left: Path, right: Path) -> str:
    manifests = [tree_manifest(left), tree_manifest(right)]
    if manifests[0] != manifests[1]:
        difference = sorted(set(manifests[0]) ^ set(manifests[1]))[:20]
        raise RuntimeError(f"{label} differ; first entries: {difference}")
    return manifest_digest(manifests[0])


@dataclass(frozen=True)
class GeneratedPass:
    root: Path
    runtime: Path
    source_plan: Path
    generated_plan: Path


def generate_pass(args: argparse.Namespace, root: Path, destination: Path) -> GeneratedPass:
    runtime = destination / "runtime"
    plans = destination / "plans"
    plans.mkdir(parents=True)

    guc_runtime_stamp = runtime / ".postgamma-guc-runtime.json"
    run(
        [
            sys.executable,
            str(root / "buildsys/generate_guc_runtime.py"),
            "--catalog",
            str(args.guc_catalog),
            "--policy",
            str(args.guc_policy),
            "--controls",
            str(args.guc_controls),
            "--api-header",
            str(root / "runtime/include/postgamma/guc_runtime.h"),
            "--output-dir",
            str(runtime),
            "--stamp",
            str(guc_runtime_stamp),
        ],
        cwd=root,
    )
    embedded_guc_policy_stamp = (
        runtime / ".postgamma-embedded-guc-policy.json"
    )
    run(
        [
            sys.executable,
            str(root / "buildsys/generate_embedded_guc_policy.py"),
            "--adapter",
            str(args.embedded_adapter),
            "--output",
            str(runtime / "include/postgamma/embedded_guc_policy.inc"),
            "--stamp",
            str(embedded_guc_policy_stamp),
        ],
        cwd=root,
    )
    backend_runtime_stamp = runtime / ".postgamma-backend-state-runtime.json"
    run(
        [
            sys.executable,
            str(root / "buildsys/generate_backend_state_runtime.py"),
            "--catalog",
            str(args.state_catalog),
            "--policy",
            str(args.state_policy),
            "--review-rules",
            str(args.state_rules),
            "--guc-inventory",
            str(args.guc_inventory),
            "--alignment",
            str(args.state_alignment),
            "--api-header",
            str(root / "runtime/include/postgamma/backend_state_runtime.h"),
            "--output-dir",
            str(runtime),
            "--stamp",
            str(backend_runtime_stamp),
        ],
        cwd=root,
    )
    tool_runtime_stamp = runtime / ".postgamma-tool-state-runtime.json"
    run(
        [
            sys.executable,
            str(root / "buildsys/generate_tool_state_runtime.py"),
            "--catalog",
            str(args.tool_state_catalog),
            "--policy",
            str(args.tool_state_policy),
            "--alignment",
            str(args.tool_state_alignment),
            "--api-header",
            str(root / "runtime/include/postgamma/tool_state_runtime.h"),
            "--output-dir",
            str(runtime),
            "--stamp",
            str(tool_runtime_stamp),
        ],
        cwd=root,
    )
    run(
        [
            sys.executable,
            str(root / "buildsys/generate_backend_execution_facts.py"),
            "--alignment",
            str(args.execution_alignment),
            "--output",
            str(runtime / "include/postgamma/backend_execution.inc"),
        ],
        cwd=root,
    )
    inheritance_report = destination / "reports/backend-inheritance.json"
    run(
        [
            sys.executable,
            str(root / "buildsys/generate_backend_inheritance.py"),
            "--catalog",
            str(args.inheritance_catalog),
            "--policy",
            str(args.inheritance_policy),
            "--runtime",
            str(backend_runtime_stamp),
            "--report",
            str(inheritance_report),
            "--facts",
            str(runtime / "include/postgamma/backend_inheritance.inc"),
        ],
        cwd=root,
    )

    source_result = run(
        [
            sys.executable,
            str(root / "buildsys/list_compile_sources.py"),
            "--database",
            str(args.compile_database),
            "--source-root",
            str(args.source_root),
            "--generated-root",
            str(args.generated_root),
            "--candidate-manifest",
            str(args.manifest),
        ],
        cwd=root,
    )
    sources = shlex.split(source_result.stdout.strip())
    if not sources:
        raise RuntimeError("candidate selection returned no source files")

    guc_source_plan = plans / "gucs.json"
    guc_generated_plan = plans / "gucs-generated.json"
    run(
        [
            str(args.tool),
            "--mode=plan",
            f"--manifest={args.manifest}",
            f"--source-root={args.source_root}",
            f"--generated-root={args.generated_root}",
            f"--output={guc_source_plan}",
            f"--generated-output={guc_generated_plan}",
            f"-p={args.compile_database.parent}",
            *sources,
        ],
        cwd=root,
    )

    state_source_plan = plans / "backend-state.json"
    state_generated_plan = plans / "backend-state-generated.json"
    run(
        [
            sys.executable,
            str(root / "buildsys/generate_backend_state_plan.py"),
            "--catalog",
            str(args.state_catalog),
            "--policy",
            str(args.state_policy),
            "--runtime",
            str(backend_runtime_stamp),
            "--source-root",
            str(args.source_root),
            "--generated-root",
            str(args.generated_root),
            "--output",
            str(state_source_plan),
            "--generated-output",
            str(state_generated_plan),
        ],
        cwd=root,
    )

    tool_state_plan = plans / "frontend-tool-state.json"
    run(
        [
            sys.executable,
            str(root / "buildsys/generate_tool_state_plan.py"),
            "--catalog",
            str(args.tool_state_catalog),
            "--policy",
            str(args.tool_state_policy),
            "--runtime",
            str(tool_runtime_stamp),
            "--source-root",
            str(args.source_root),
            "--embedded-adapter",
            str(args.embedded_adapter),
            "--output",
            str(tool_state_plan),
        ],
        cwd=root,
    )
    context_state_plan = plans / "context-state.json"
    run(
        [
            sys.executable,
            str(root / "buildsys/reconcile_context_state_plans.py"),
            "--backend-plan",
            str(state_source_plan),
            "--tool-plan",
            str(tool_state_plan),
            "--output",
            str(context_state_plan),
        ],
        cwd=root,
    )

    source_plan = plans / "postgres.json"
    generated_plan = plans / "postgres-generated.json"
    for inputs, output in (
        ((guc_source_plan, context_state_plan), source_plan),
        ((guc_generated_plan, state_generated_plan), generated_plan),
    ):
        run(
            [
                sys.executable,
                str(root / "buildsys/merge_replacement_plans.py"),
                "--plan",
                str(inputs[0]),
                "--plan",
                str(inputs[1]),
                "--output",
                str(output),
            ],
            cwd=root,
        )
    return GeneratedPass(destination, runtime, source_plan, generated_plan)


def install_generated_artifacts(
    root: Path, plan: Path, template_root: Path, destination: Path
) -> None:
    destination.mkdir(parents=True)
    (destination / ".postgamma-configure.json").write_text("{}\n", encoding="utf-8")
    run(
        [
            sys.executable,
            str(root / "buildsys/install_generated_artifacts.py"),
            "--plan",
            str(plan),
            "--template-root",
            str(template_root),
            "--tree",
            str(destination),
            "--build-root",
            str((root / "build").resolve()),
        ],
        cwd=root,
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--project-root", required=True, type=Path)
    parser.add_argument("--upstream-manifest", required=True, type=Path)
    parser.add_argument("--adapter", required=True, type=Path)
    parser.add_argument("--embedded-adapter", required=True, type=Path)
    parser.add_argument(
        "--support-profile", choices=("product", "validation"), default="validation"
    )
    parser.add_argument("--tool", required=True, type=Path)
    parser.add_argument("--compile-database", required=True, type=Path)
    parser.add_argument("--manifest", required=True, type=Path)
    parser.add_argument("--source-root", required=True, type=Path)
    parser.add_argument("--generated-root", required=True, type=Path)
    parser.add_argument("--guc-catalog", required=True, type=Path)
    parser.add_argument("--guc-policy", required=True, type=Path)
    parser.add_argument("--guc-controls", required=True, type=Path)
    parser.add_argument("--guc-inventory", required=True, type=Path)
    parser.add_argument("--state-catalog", required=True, type=Path)
    parser.add_argument("--state-policy", required=True, type=Path)
    parser.add_argument("--state-rules", required=True, type=Path)
    parser.add_argument("--state-alignment", required=True, type=Path)
    parser.add_argument("--tool-state-catalog", required=True, type=Path)
    parser.add_argument("--tool-state-policy", required=True, type=Path)
    parser.add_argument("--tool-state-alignment", required=True, type=Path)
    parser.add_argument("--execution-alignment", required=True, type=Path)
    parser.add_argument("--inheritance-catalog", required=True, type=Path)
    parser.add_argument("--inheritance-policy", required=True, type=Path)
    parser.add_argument("--build-root", required=True, type=Path)
    args = parser.parse_args()

    root = args.project_root.resolve()
    for attribute in (
        "tool",
        "upstream_manifest",
        "adapter",
        "embedded_adapter",
        "compile_database",
        "manifest",
        "source_root",
        "generated_root",
        "guc_catalog",
        "guc_policy",
        "guc_controls",
        "guc_inventory",
        "state_catalog",
        "state_policy",
        "state_rules",
        "state_alignment",
        "tool_state_catalog",
        "tool_state_policy",
        "tool_state_alignment",
        "execution_alignment",
        "inheritance_catalog",
        "inheritance_policy",
    ):
        setattr(args, attribute, getattr(args, attribute).resolve())
    build = args.build_root.resolve()
    require_build_child(build, root)
    if build.exists():
        shutil.rmtree(build)
    build.mkdir(parents=True)

    passes = [
        generate_pass(args, root, build / "pass-one"),
        generate_pass(args, root, build / "pass-two"),
    ]
    for relative in (
        "runtime",
        "plans/gucs.json",
        "plans/gucs-generated.json",
        "plans/backend-state.json",
        "plans/backend-state-generated.json",
        "plans/frontend-tool-state.json",
        "plans/context-state.json",
        "plans/postgres.json",
        "plans/postgres-generated.json",
        "reports/backend-inheritance.json",
    ):
        left = passes[0].root / relative
        right = passes[1].root / relative
        if left.is_dir():
            require_equal_trees(relative, left, right)
        elif left.read_bytes() != right.read_bytes():
            raise RuntimeError(f"independently generated {relative} files differ")

    trees = [build / "tree-one", build / "tree-two"]
    for generated, tree in zip(passes, trees):
        generate_tree(
            root,
            args.source_root,
            args.upstream_manifest,
            args.adapter,
            generated.source_plan,
            tree,
            generated.runtime,
            args.support_profile,
        )
    source_digest = require_equal_trees("generated PostgreSQL source trees", *trees)

    artifact_trees = [build / "artifacts-one", build / "artifacts-two"]
    for generated, tree in zip(passes, artifact_trees):
        install_generated_artifacts(
            root, generated.generated_plan, args.generated_root, tree
        )
    artifact_digest = require_equal_trees(
        "configured-build artifact trees", *artifact_trees
    )

    print(
        "PostGamma generation determinism: runtime, inheritance report, and 8 plans "
        "identical; "
        f"source_sha256={source_digest}; artifact_sha256={artifact_digest}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
