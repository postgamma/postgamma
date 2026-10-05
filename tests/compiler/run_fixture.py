#!/usr/bin/env python3
"""End-to-end semantic AST, generation, and runtime fixture."""

from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
from collections import Counter
from pathlib import Path


def run(arguments: list[str], *, cwd: Path, expected: int = 0) -> subprocess.CompletedProcess[str]:
    result = subprocess.run(
        arguments, cwd=cwd, text=True,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE,
    )
    if result.returncode != expected:
        raise RuntimeError(
            f"command returned {result.returncode}, expected {expected}: {arguments}\n"
            f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        )
    return result


def invoke_tool(tool: Path, mode: str, manifest: Path, source_root: Path,
                source: Path | list[Path], output: Path, *, expected: int = 0,
                extra_tool_args: tuple[str, ...] = ()) -> subprocess.CompletedProcess[str]:
    sources = [source] if isinstance(source, Path) else source
    return run(
        [
            str(tool), f"--mode={mode}", f"--manifest={manifest}",
            f"--source-root={source_root}", f"--output={output}",
            *extra_tool_args,
            *(str(item) for item in sources), "--", "-xc", "-std=c11",
        ],
        cwd=source_root,
        expected=expected,
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--project-root", required=True, type=Path)
    parser.add_argument("--tool", required=True, type=Path)
    parser.add_argument("--state-tool", required=True, type=Path)
    parser.add_argument("--inheritance-tool", required=True, type=Path)
    parser.add_argument("--runtime-dir", required=True, type=Path)
    parser.add_argument("--build-dir", required=True, type=Path)
    args = parser.parse_args()
    root = args.project_root.resolve()
    tool = args.tool.resolve()
    state_tool = args.state_tool.resolve()
    inheritance_tool = args.inheritance_tool.resolve()
    runtime_dir = args.runtime_dir.resolve()
    build = args.build_dir.resolve()
    fixtures = root / "tests/compiler/fixtures"
    source = fixtures / "guc_uses.c"
    manifest = fixtures / "guc-fixture.json"
    if build.exists():
        shutil.rmtree(build)
    build.mkdir(parents=True)

    scan_path = build / "scan.json"
    plan_one = build / "plan-one.json"
    plan_two = build / "plan-two.json"
    invoke_tool(tool, "scan", manifest, fixtures, source, scan_path)
    scan = json.loads(scan_path.read_text(encoding="utf-8"))
    if scan["summary"]["use_count"] != 8:
        raise RuntimeError(f"expected 8 global uses, got {scan['summary']}")
    kinds = Counter(record["use_kind"] for record in scan["uses"])
    expected_kinds = Counter({"read": 2, "write": 2, "read_write": 2, "address": 2})
    if kinds != expected_kinds:
        raise RuntimeError(f"unexpected use classification: {kinds}")
    if any(record["macro"] for record in scan["uses"]):
        raise RuntimeError("ordinary fixture unexpectedly contains a macro use")

    invoke_tool(tool, "plan", manifest, fixtures, source, plan_one)
    invoke_tool(tool, "plan", manifest, fixtures, source, plan_two)
    if plan_one.read_bytes() != plan_two.read_bytes():
        raise RuntimeError("two identical AST generations produced different plans")

    generated = build / "generated"
    generated.mkdir()
    (generated / ".postgamma-source.json").write_text("{}\n", encoding="utf-8")
    shutil.copy2(source, generated / source.name)
    run(
        [
            os.environ.get("PYTHON", "python3"),
            str(root / "buildsys/apply_replacements.py"),
            "--plan", str(plan_one), "--tree", str(generated),
        ],
        cwd=root,
    )
    transformed = (generated / source.name).read_text(encoding="utf-8")
    if transformed.count("POSTGAMMA_GUC_VALUE(") != 8:
        raise RuntimeError("generated fixture does not contain exactly 8 semantic replacements")
    if "settings->work_mem += 1" not in transformed or "int work_mem" not in transformed:
        raise RuntimeError("AST transformation modified a local or member with the same spelling")

    executable = build / "generated-fixture"
    run(
        [
            os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
            "-D_GNU_SOURCE", "-pthread",
            "-I", str(root / "runtime/include"),
            "-I", str(runtime_dir / "include"),
            "-include", str(runtime_dir / "include/postgamma/guc_runtime.h"),
            str(generated / source.name),
            str(fixtures / "guc_harness.c"),
            str(root / "runtime/src/thread_runtime.c"),
            str(root / "runtime/src/execution_context.c"),
            str(root / "runtime/src/backend_state_runtime.c"),
            "-o", str(executable),
        ],
        cwd=root,
    )
    run([str(executable)], cwd=root)

    macro_source = fixtures / "guc_macro.c"
    macro_manifest = fixtures / "guc-macro.json"
    macro_scan = build / "macro-scan.json"
    invoke_tool(tool, "scan", macro_manifest, fixtures, macro_source, macro_scan)
    macro_document = json.loads(macro_scan.read_text(encoding="utf-8"))
    if macro_document["summary"]["macro_use_count"] != 1:
        raise RuntimeError("macro use was not identified")
    macro_plan = build / "macro-plan.json"
    invoke_tool(tool, "plan", macro_manifest, fixtures, macro_source, macro_plan)
    macro_plan_document = json.loads(macro_plan.read_text(encoding="utf-8"))
    macro_replacements = [
        replacement
        for file_record in macro_plan_document["files"]
        for replacement in file_record["replacements"]
    ]
    if len(macro_replacements) != 1 or macro_replacements[0]["use_kind"] != "macro_body":
        raise RuntimeError("macro-body expansions were not coalesced into one audited edit")
    invoke_tool(
        tool, "scan", fixtures / "guc-type-drift.json", fixtures,
        macro_source, build / "type-drift-scan.json", expected=3,
    )

    binding_scan = build / "binding-scan.json"
    binding_sources = [
        fixtures / "binding_backend.c",
        fixtures / "binding_frontend.c",
    ]
    invoke_tool(
        tool,
        "scan",
        fixtures / "guc-binding-anchor.json",
        fixtures,
        binding_sources,
        binding_scan,
    )
    binding_document = json.loads(binding_scan.read_text(encoding="utf-8"))
    if binding_document["summary"]["use_count"] != 1:
        raise RuntimeError(
            "generated-table binding did not remove the unrelated global collision"
        )
    if binding_document["uses"][0]["path"] != "binding_backend.c":
        raise RuntimeError("generated-table binding selected the wrong canonical variable")

    injection_source = fixtures / "injection.c"
    injection_manifest = fixtures / "injection.json"
    injection_plan = build / "injection-plan.json"
    invoke_tool(
        tool, "plan", injection_manifest, fixtures, injection_source,
        injection_plan,
    )
    injection_document = json.loads(injection_plan.read_text(encoding="utf-8"))
    if injection_document["summary"]["injection_count"] != 5:
        raise RuntimeError("semantic injection anchors were not uniquely identified")
    if injection_document["assumption_diagnostics"] != [
        {
            "actual_matches": 1,
            "callee": "MemoryContextInit",
            "expected_matches": 1,
            "id": "assume.memory-context-entry",
            "matches": [
                {
                    "allowed": True,
                    "column": 2,
                    "enclosing_function": "main",
                    "line": 75,
                    "path": "injection.c",
                }
            ],
            "status": "ok",
        }
    ]:
        raise RuntimeError("execution-model assumption was not enforced")
    injection_generated = build / "injection-generated"
    injection_generated.mkdir()
    (injection_generated / ".postgamma-source.json").write_text(
        "{}\n", encoding="utf-8"
    )
    shutil.copy2(injection_source, injection_generated / injection_source.name)
    run(
        [
            os.environ.get("PYTHON", "python3"),
            str(root / "buildsys/apply_replacements.py"),
            "--plan", str(injection_plan), "--tree", str(injection_generated),
        ],
        cwd=root,
    )
    injection_text = (injection_generated / injection_source.name).read_text(
        encoding="utf-8"
    )
    expected_injections = (
        "\tpostgamma_at_entry();\n\ttrace += 0;",
        "#ifndef FRONTEND\n\tpostgamma_before_anchor();\n\ttrace += 0;\n#endif",
        "\tpostgamma_after_anchor();\n\ttrace += 0;",
        "\tint\t\t\tvalue = LookupValue();\n\tpostgamma_after_declaration();",
        "\tif (trace >= 0)\n\t\tConditionalAnchor();\n\tpostgamma_after_conditional();",
    )
    if any(snippet not in injection_text for snippet in expected_injections):
        raise RuntimeError("multi-line semantic injection lost source indentation")
    if "\t#ifndef FRONTEND" in injection_text or "\t#endif" in injection_text:
        raise RuntimeError("semantic injection indented a preprocessor directive")
    if injection_text.index("expected = 4123") > injection_text.index(
        "postgamma_at_entry();"
    ):
        raise RuntimeError("function-entry injection preceded local declarations")
    injection_executable = build / "injection-fixture"
    run(
        [
            os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
            "-Werror=declaration-after-statement",
            str(injection_generated / injection_source.name), "-o", str(injection_executable),
        ],
        cwd=root,
    )
    run([str(injection_executable)], cwd=root)
    invoke_tool(
        tool, "plan", fixtures / "injection-stale.json", fixtures,
        injection_source, build / "injection-stale-plan.json", expected=3,
    )
    injection_probe = build / "injection-stale-probe.json"
    invoke_tool(
        tool, "scan", fixtures / "injection-stale.json", fixtures,
        injection_source, injection_probe,
        extra_tool_args=("--report-injection-incompatibility",),
    )
    diagnostics = json.loads(
        injection_probe.read_text(encoding="utf-8")
    )["injection_diagnostics"]
    if diagnostics != [
        {
            "actual_matches": 1,
            "errors": [],
            "expected_matches": 2,
            "id": "fixture.stale-anchor",
            "status": "incompatible",
        }
    ]:
        raise RuntimeError(f"unexpected injection probe diagnostics: {diagnostics}")

    zero_manifest = fixtures / "assumption-zero.json"
    zero_clean = fixtures / "assumption_zero_clean.c"
    zero_forbidden = fixtures / "assumption_zero_forbidden.c"
    zero_scan = build / "assumption-zero-scan.json"
    invoke_tool(tool, "scan", zero_manifest, fixtures, zero_clean, zero_scan)
    zero_diagnostics = json.loads(
        zero_scan.read_text(encoding="utf-8")
    )["assumption_diagnostics"]
    if zero_diagnostics != [
        {
            "actual_matches": 0,
            "callee": "forbidden_process_signal",
            "expected_matches": 0,
            "id": "assume.no-process-signal",
            "matches": [],
            "status": "ok",
        }
    ]:
        raise RuntimeError(
            f"unexpected zero-match assumption diagnostics: {zero_diagnostics}"
        )
    failure = invoke_tool(
        tool,
        "scan",
        zero_manifest,
        fixtures,
        [zero_clean, zero_forbidden],
        build / "assumption-zero-failure.json",
        expected=3,
    )
    if (
        "assumption_zero_forbidden.c:" not in failure.stderr
        or "function exercise_forbidden_assumption" not in failure.stderr
    ):
        raise RuntimeError(
            "zero-match assumption failure omitted source/function diagnostics"
        )

    state_source = fixtures / "backend_state.c"
    state_consumer = fixtures / "backend_state_consumer.c"
    state_catalog_path = build / "backend-state.json"
    state_domain_path = build / "backend-state-domain.json"
    state_domain_path.write_text(
        json.dumps(
            {
                "schema_version": 1,
                "kind": "postgamma.postgres-source-domain",
                "translation_unit_prefixes": [],
                "translation_unit_files": ["backend_state.c"],
            },
            indent=2,
            sort_keys=True,
        )
        + "\n",
        encoding="utf-8",
    )
    run(
        [
            str(state_tool),
            f"--source-root={fixtures}",
            f"--definition-domain={state_domain_path}",
            f"--output={state_catalog_path}",
            str(state_source),
            str(state_consumer),
            "--",
            "-xc",
            "-std=c11",
        ],
        cwd=fixtures,
    )
    state_catalog = json.loads(state_catalog_path.read_text(encoding="utf-8"))
    candidates = {candidate["name"]: candidate for candidate in state_catalog["candidates"]}
    if set(candidates) != {
        "anonymous_state",
        "dependent_state",
        "external_state",
        "function_state",
        "internal_state",
    }:
        raise RuntimeError(f"unexpected mutable-state candidates: {sorted(candidates)}")
    if str(fixtures) in candidates["anonymous_state"]["canonical_type"]:
        raise RuntimeError("anonymous canonical type leaked its source-root path")
    if not candidates["function_state"]["function_static"]:
        raise RuntimeError("function-static state was not classified")
    external_uses = [
        use
        for use in state_catalog["uses"]
        if use["id"] == "external:external_state"
        and use["path"] == "backend_state_consumer.c"
    ]
    if len(external_uses) != 1:
        raise RuntimeError(
            "an out-of-domain consumer of core external state was not inventoried"
        )
    external_references = state_catalog.get("external_references")
    if (
        state_catalog["summary"].get("external_reference_count") != 1
        or not isinstance(external_references, list)
        or len(external_references) != 1
        or external_references[0]["id"] != "external:kernel_state"
        or external_references[0]["path"] != "backend_state_consumer.c"
    ):
        raise RuntimeError(
            "a declaration-only mutable kernel reference was not inventoried"
        )
    initializer_uses = [
        use for use in state_catalog["uses"] if use["in_static_initializer"]
    ]
    if len(initializer_uses) != 1 or not initializer_uses[0]["static_initializer_owner_id"].endswith(
        "::dependent_state"
    ):
        raise RuntimeError("static-initializer ownership was not recorded")

    frontend_state_source = fixtures / "frontend_tool_state.c"
    frontend_state_catalog_path = build / "frontend-tool-state.json"
    frontend_state_domain_path = build / "frontend-tool-state-domain.json"
    frontend_state_domain_path.write_text(
        json.dumps(
            {
                "schema_version": 1,
                "kind": "postgamma.postgres-source-domain",
                "translation_unit_prefixes": [],
                "translation_unit_files": ["frontend_tool_state.c"],
            },
            indent=2,
            sort_keys=True,
        )
        + "\n",
        encoding="utf-8",
    )
    run(
        [
            str(state_tool),
            f"--source-root={fixtures}",
            f"--definition-domain={frontend_state_domain_path}",
            f"--output={frontend_state_catalog_path}",
            str(frontend_state_source),
            "--",
            "-xc",
            "-std=c11",
            "-DFRONTEND",
        ],
        cwd=fixtures,
    )
    frontend_state_catalog = json.loads(
        frontend_state_catalog_path.read_text(encoding="utf-8")
    )
    frontend_ids = {
        candidate["id"] for candidate in frontend_state_catalog["candidates"]
    }
    if frontend_ids != {
        "external:frontend_library_state",
        "internal:frontend_tool_state.c:frontend_tool_state.c::frontend_tool_buffer",
    }:
        raise RuntimeError(
            f"unexpected frontend/tool state identities: {sorted(frontend_ids)}"
        )

    inheritance_source = fixtures / "backend_inheritance.c"
    inheritance_catalog_path = build / "backend-inheritance.json"
    run(
        [
            str(inheritance_tool),
            f"--source-root={fixtures}",
            "--contract-function=save_backend_variables",
            f"--output={inheritance_catalog_path}",
            str(inheritance_source),
            "--",
            "-xc",
            "-std=c11",
        ],
        cwd=fixtures,
    )
    inheritance_catalog = json.loads(
        inheritance_catalog_path.read_text(encoding="utf-8")
    )
    inherited_ids = {state["id"] for state in inheritance_catalog["states"]}
    if inherited_ids != {
        "external:inherited_external",
        "internal:backend_inheritance.c:backend_inheritance.c::inherited_internal",
    }:
        raise RuntimeError(
            f"unexpected backend inheritance states: {sorted(inherited_ids)}"
        )

    print(
        "compiler fixture: semantic binding, generated-table identity, deterministic "
        "plan, semantic anchors, backend and frontend/tool state identities, "
        "inheritance inventories, "
        "and fail-closed checks ok"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
