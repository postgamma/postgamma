#!/usr/bin/env python3
"""Generate target state layouts with the shared host AST pipeline."""

from __future__ import annotations

import argparse
import copy
import os
from pathlib import Path
import shlex
import sys

from filter_compile_database import select_compile_records
from wasm_kernel import build_target, clean_environment
from wasm_source import source_tree, apply_adaptations, SHMEM_HEADER, SHMEM_MARKER, SHMEM_REPLACEMENT
from wasm_toolchain import (
    WasmError, capture, configuration, digest, fingerprint, read_json, run,
    run_id, sdk_tools, write_json,
)


def project_adapter(adapter: dict, adaptations: list[dict]) -> dict:
    """Keep the shared installer strict after the target default was adapted."""
    result = copy.deepcopy(adapter)
    if not any(row['path'] == SHMEM_HEADER for row in adaptations):
        return result
    matches = [row for row in result['generated_support']['edits'] if row['id'] == 'shmem.target-default']
    expected = {'id': 'shmem.target-default', 'path': SHMEM_HEADER, 'mode': 'replace',
                'match': SHMEM_MARKER.splitlines()[0], 'expected_matches': 1,
                'content': '#if defined(__EMSCRIPTEN__) || (!defined(WIN32) && !defined(EXEC_BACKEND))',
                'reject_if_present': '#if defined(__EMSCRIPTEN__) ||'}
    if matches != [expected]:
        raise WasmError('unreviewed shared-memory default support adapter')
    # The installer still checks and replaces exactly one reviewed guard. Both
    # forms choose mmap for this target; native generation uses the original.
    matches[0]['match'] = SHMEM_REPLACEMENT.splitlines()[0]
    return result


def project_policy(policy: dict, catalog: dict, scope: dict) -> tuple[dict, list[dict]]:
    """Apply explicit target availability without changing shared ownership."""
    result = copy.deepcopy(policy)
    candidates = {entry["id"] for entry in catalog["candidates"]}
    decisions = {entry["id"] for entry in policy["decisions"]}
    missing = sorted(candidates - decisions)
    if missing:
        raise WasmError("target candidates need shared ownership review: " + ", ".join(missing))
    excluded = []
    unknown = []
    for decision in result["decisions"]:
        identifier = decision["id"]
        if identifier in candidates or decision.get("availability") == "conditional":
            continue
        reason = scope.get("identifiers", {}).get(identifier)
        if not reason and not identifier.startswith("external:"):
            for prefix, explanation in scope.get("definition_prefixes", {}).items():
                if identifier.split(":", 2)[1].startswith(prefix):
                    reason = explanation
                    break
        if not reason:
            unknown.append(identifier)
            continue
        decision.update(availability="conditional", condition=reason)
        excluded.append({"id": identifier, "condition": reason})
    if unknown:
        raise WasmError("unreviewed target policy exclusions: " + ", ".join(unknown))
    return result, excluded


def host_ast_tools(config, log: Path) -> Path:
    finder = config.root / "buildsys/find_tool.py"
    names = {key: capture([sys.executable, str(finder), kind, "--root", str(config.root)])
             for key, kind in (("LLVM_CONFIG", "llvm-config"), ("CLANGXX", "clang++"),
                               ("CLANG_CPP_LIBRARY", "clang-cpp-library"),
                               ("CLANG_RESOURCE_DIR", "clang-resource-dir"))}
    identity = {"tools": names, "llvm_version": capture([names["LLVM_CONFIG"], "--version"]),
                "sources": {str(p.relative_to(config.root)): digest(p)
                            for p in sorted((config.root / "compiler").rglob("*")) if p.is_file()}}
    directory = config.build / "wasm/host-tools" / fingerprint(identity)
    run(["make", "-j1", "-C", str(config.root / "compiler"), f"BUILD_DIR={directory}",
         *(key + "=" + value for key, value in names.items())], log, clean_environment(dict(os.environ)))
    write_json(directory / "inputs.json", identity)
    return directory


def check_layout(config, directory: Path, host: Path, tools: dict, env: dict,
                 resource: str, log: Path) -> dict:
    postgres = source_tree(config, directory, read_json(directory / "inputs.json"))
    source = config.root / "tests/wasm/fixtures/target-layout.c"
    output = directory / "inventory/abi"
    build = directory / "configure"
    fragments_dir = output / "fragments" / run_id()
    sdk_env = dict(env, EM_COMPILER_WRAPPER=str(config.root / "buildsys/wasm_cc_capture.py"),
                   POSTGAMMA_CAPTURE_DIR=str(fragments_dir))
    output.mkdir(parents=True, exist_ok=True)
    run([str(config.sdk / "upstream/emscripten/emcc"), "-pthread", "-std=gnu11", "-Werror",
         "-sSUPPORT_LONGJMP=emscripten", "-DEXEC_BACKEND", f"-I{build / 'src/include'}",
         f"-I{postgres / 'src/include'}", "-c", str(source), "-o", str(output / "layout.o")],
        log, sdk_env, config.root)
    fragments = [read_json(p) for p in sorted(fragments_dir.glob("*.json"))]
    if len(fragments) != 1:
        raise WasmError("ABI fixture must capture exactly one target compile command")
    # This directory contains only the fixture's actual target compile command.
    write_json(output / "compile_commands.json", [fragments[-1]])
    catalog = output / "layout.json"
    domain = read_json(config.root / "manifests/domains/postgres-server.json")
    domain.update(id="wasm-target-layout", description="Target compiler and AST layout agreement",
                  translation_unit_prefixes=[], translation_unit_files=[source.name],
                  binding_anchor_file_suffixes=[])
    write_json(output / "domain.json", domain)
    run([str(host / "postgamma-state-inventory"), f"--source-root={source.parent}",
         f"--definition-domain={output / 'domain.json'}",
         f"--clang-resource-dir={resource}", f"--output={catalog}", f"-p={output}", str(source)],
        log, env, config.root)
    values = {entry["name"]: entry for entry in read_json(catalog)["candidates"]}
    expected = {"postgamma_layout_pointer": (4, 4), "postgamma_layout_long": (4, 4),
                "postgamma_layout_datum": (8, 8),
                "postgamma_layout_record": (32, 8), "postgamma_layout_callback": (4, 4),
                "postgamma_layout_tls": (8, 8)}
    for name, (size, alignment) in expected.items():
        value = values[name]
        if value["size"] != size or value["alignment"] != alignment:
            raise WasmError(f"host AST layout differs from target compiler: {name}")
    if values["postgamma_layout_tls"]["tls_kind"] == "none":
        raise WasmError("host AST did not preserve target TLS storage")
    return {"fixture_sha256": digest(source), "object_sha256": digest(output / "layout.o"),
            "catalog_sha256": digest(catalog), "layout": expected,
            "tls_kind": values["postgamma_layout_tls"]["tls_kind"],
            "callback_type": values["postgamma_layout_callback"]["canonical_type"]}


def tree_hashes(directory: Path) -> dict:
    return {str(path.relative_to(directory)): digest(path)
            for path in sorted(directory.rglob("*")) if path.is_file()}


def generation_sources(root: Path) -> dict:
    return {"sources": {name: tree_hashes(root / name) for name in
                        ("runtime", "manifests", "embedded-c/include")},
            "generators": {str(p.relative_to(root)): digest(p)
                           for p in sorted((root / "buildsys").glob("*.py"))}}


def verify_generation_sources(root: Path, inputs: dict) -> None:
    if generation_sources(root) != {key: inputs[key] for key in ("sources", "generators")}:
        raise WasmError("generation inputs changed while running; rerun with stable inputs")


def check_generator_tools(config):
    """Reject an incompatible host frontend before compiling the kernel."""
    current = config.build / 'wasm/generator-tools' / run_id()
    report = dict(kind='postgamma.wasm-generator-tools', status='failed')
    try:
        tools, env = sdk_tools(config)
        finder = config.root / 'buildsys/find_tool.py'
        clang = capture([sys.executable, str(finder), 'clang++', '--root', str(config.root)])
        resource = capture([str(Path(tools['settings']['LLVM_ROOT']) / 'clang'), '-print-resource-dir'], env)
        source = config.root / 'tests/wasm/fixtures/sdk-headers.c'
        command = [clang, '-x', 'c', '--target=wasm32-unknown-emscripten', '-std=gnu11',
                   '--sysroot=' + str(Path(tools['settings']['CACHE']) / 'sysroot'),
                   '-resource-dir=' + resource, '-pthread', '-Werror', '-fsyntax-only', str(source)]
        report.update(command=command, clang_version=capture([clang, '--version']).splitlines()[0],
                      clang_sha256=digest(Path(clang)), fixture_sha256=digest(source))
        run(command, current / 'headers.log', env, config.root, timeout=30)
        report['status'] = 'passed'
    except Exception as error:
        report['error'] = str(error)
        raise WasmError('Host AST frontend cannot parse the selected SDK headers; select a compatible LLVM/Clang pair. See ' + str(current / 'headers.log')) from error
    finally:
        write_json(current / 'report.json', report)
        print(f"wasm generator tools: {report['status']}; {current / 'report.json'}", flush=True)
    return current / 'report.json'


def ast_cached(command: list[str], outputs: list[Path], inputs: dict,
               log: Path, env: dict, root: Path) -> None:
    stamp = outputs[0].with_suffix(".ast-receipt.json")
    identity = {"command": command, "inputs": inputs}
    if stamp.is_file():
        previous = read_json(stamp)
        if (previous.get("status") == "passed" and previous.get("identity") == identity and
                previous.get("outputs") == {str(p): digest(p) for p in outputs if p.is_file()}):
            return
    write_json(stamp, {"status": "failed", "identity": identity})
    run(command, log, env, root)
    write_json(stamp, {"status": "passed", "identity": identity,
                       "outputs": {str(p): digest(p) for p in outputs}})


def generate(config, longjmp: str, candidate_probe: bool = False) -> Path:
    generator_tools = check_generator_tools(config)
    directory = build_target(config, "capture", longjmp)
    tools, environment = sdk_tools(config)
    env = clean_environment(environment)
    current = directory / "runs" / ("generate-" + run_id())
    log = current / "build.log"
    report = {"kind": "postgamma.wasm-kernel-generation", "schema_version": 1,
              "status": "failed", "kernel_linked": False, "sql_validated": False, "candidate_probe": candidate_probe,
              "generator_tools": {'path': str(generator_tools), 'sha256': digest(generator_tools)}}
    root = config.root
    kernel_inputs = read_json(directory / "inputs.json")
    source, build = source_tree(config, directory, kernel_inputs), directory / "configure"
    inventory, plans = directory / "inventory", directory / "plans"
    runtime = directory / "generated/runtime"
    reports = directory / "reports"
    target_policy = read_json(root / "manifests/profiles/wasm-state-scope.json")

    def script(name: str, **arguments) -> None:
        command = [sys.executable, str(root / "buildsys" / (name + ".py"))]
        for key, value in arguments.items():
            if isinstance(value, bool):
                if value: command += ["--" + key.replace("_", "-")]
                continue
            for item in value if isinstance(value, list) else [value]:
                command += ["--" + key.replace("_", "-"), str(item)]
        run(command, log, env, root)

    def selected(database: Path, **options) -> list[str]:
        command = [sys.executable, str(root / "buildsys/list_compile_sources.py"),
                   "--database", str(database), "--source-root", str(source),
                   "--generated-root", str(build)]
        for key, value in options.items():
            command += ["--" + key.replace("_", "-"), str(value)]
        return shlex.split(capture(command, env))

    try:
        host = host_ast_tools(config, log)
        resource = capture([str(Path(tools["settings"]["LLVM_ROOT"]) / "clang"),
                            "-print-resource-dir"], environment)
        report["host_tools"] = {name: {"path": str(host / name), "sha256": digest(host / name)}
                                for name in ("postgamma-cc", "postgamma-state-inventory",
                                             "postgamma-inheritance-inventory")}
        report["target_resource_directory"] = resource
        report["abi"] = check_layout(config, directory, host, tools, env, resource, log)
        ast_inputs = {"tools": report["host_tools"],
                      "database_sha256": digest(directory / "compile-db/compile_commands.json"),
                      "target_headers": tree_hashes(build / "src/include"),
                      "sysroot_headers": fingerprint(tree_hashes(Path(tools["settings"]["CACHE"]) / "sysroot/include")),
                      "builtin_headers": fingerprint(tree_hashes(Path(resource) / "include"))}
        report["inputs"] = {"ast": ast_inputs, **generation_sources(root),
                            "fixture": report["abi"]["fixture_sha256"]}
        databases = {}
        catalogs = {}
        for name, domain in (("backend", "postgres-server"), ("tool", "postgres-frontend-tool")):
            manifest = root / "manifests/domains" / (domain + ".json")
            rows, selection = select_compile_records(
                read_json(directory / "compile-db/compile_commands.json"),
                source, build, read_json(manifest))
            database = directory / "compile-db" / name / "compile_commands.json"
            write_json(database, rows)
            write_json(inventory / (name + "-selection.json"), selection)
            output = inventory / (name + "-state.json")
            ast_cached([str(host / "postgamma-state-inventory"), f"--source-root={source}",
                 f"--generated-root={build}", f"--definition-domain={manifest}",
                 f"--clang-resource-dir={resource}", f"--output={output}", f"-p={database.parent}",
                 *selected(database, domain_manifest=manifest)], [output], ast_inputs, log, env, root)
            databases[name], catalogs[name] = database, output

        shared_adapter = root / "manifests/postgresql/adapter.json"
        adapter = inventory / "postgresql-adapter.json"
        write_json(adapter, project_adapter(read_json(shared_adapter), kernel_inputs["source_adaptations"]))
        report["support_adapter"] = {"shared_sha256": digest(shared_adapter), "target_sha256": digest(adapter)}
        facts = read_json(adapter)["fact_sources"]
        for fact, generator, label in (("guc_catalog", "generate_guc_catalog", "gucs"),
                                        ("backend_execution_catalog", "generate_backend_execution_catalog", "executions")):
            entry = facts[fact]
            script(generator, input=source / entry["path"], source_label=entry["path"],
                   parser=entry["parser"], output=inventory / (label + ".json"))
        gucs = inventory / "gucs.json"
        guc_policy = root / "manifests/ownership/gucs.json"
        guc_controls = root / "manifests/ownership/guc-control.json"
        guc_manifest = inventory / "guc-transform.json"
        script("check_guc_policy", catalog=gucs, policy=guc_policy, report=reports / "guc-ownership.json")
        script("generate_guc_runtime", catalog=gucs, policy=guc_policy, controls=guc_controls,
               api_header=root / "runtime/include/postgamma/guc_runtime.h", output_dir=runtime,
               stamp=runtime / ".postgamma-guc-runtime.json")
        script("generate_embedded_guc_policy", adapter=root / "manifests/postgresql/embedded.json",
               output=runtime / "include/postgamma/embedded_guc_policy.inc",
               stamp=runtime / ".postgamma-embedded-guc-policy.json")
        script("generate_guc_transform_manifest", catalog=gucs, policy=guc_policy, adapter=adapter,
               domain=root / "manifests/domains/postgres-server.json", controls=guc_controls,
               output=guc_manifest)
        ast = [str(host / "postgamma-cc"), f"--manifest={guc_manifest}", f"--source-root={source}",
               f"--generated-root={build}", f"--clang-resource-dir={resource}",
               f"-p={databases['backend'].parent}",
               *selected(databases["backend"], candidate_manifest=guc_manifest)]
        guc_inventory = inventory / "guc-ast.json"
        guc_inputs = {**ast_inputs, "manifest_sha256": digest(guc_manifest)}
        ast_cached([*ast, "--mode=scan", f"--output={guc_inventory}"], [guc_inventory], guc_inputs, log, env, root)

        from project_guc_storage import project as project_guc_storage
        excluded = {}
        policies = {}
        for name, filename in (("backend", "backend-state.json"), ("tool", "frontend-tool-state.json")):
            shared_policy = read_json(root / "manifests/ownership" / filename)
            if name == "backend":
                shared_policy = project_guc_storage(shared_policy, read_json(catalogs[name]),
                    read_json(guc_inventory), read_json(root / "manifests/ownership/backend-guc-transitions.json"))
            policy, excluded[name] = project_policy(shared_policy, read_json(catalogs[name]), target_policy[name])
            policies[name] = inventory / (name + "-policy.json")
            write_json(policies[name], policy)
        rules = read_json(root / "manifests/ownership/backend-state-rules.json")
        indexed = {entry["id"]: entry for entry in rules["rules"]}
        for entry in excluded["backend"]:
            rule = indexed.setdefault(entry["id"], {"id": entry["id"]})
            rule.update(availability="conditional", condition=entry["condition"])
        rules["rules"] = list(indexed.values())
        rules_path = inventory / "backend-rules.json"
        write_json(rules_path, rules)
        write_json(reports / "target-state-availability.json", excluded)
        script("check_backend_state_policy", catalog=catalogs["backend"], policy=policies["backend"],
               guc_inventory=guc_inventory, review_rules=rules_path, report=reports / "backend-ownership.json")
        from check_session_mobility import stable_digest
        mobility = read_json(root / "manifests/ownership/backend-state-mobility.json")
        shared = read_json(root / "manifests/ownership/backend-state.json")["decisions"]
        expected_review = {"candidate_ids_sha256": stable_digest(sorted(row["id"] for row in shared)),
                           "ownership_decisions_sha256": stable_digest(sorted(shared, key=lambda row: row["id"]))}
        if mobility["review_fingerprint"] != expected_review:
            raise WasmError("shared mobility review does not match current ownership")
        projected = read_json(policies["backend"])["decisions"]
        mobility["review_fingerprint"] = {
            "candidate_ids_sha256": stable_digest(sorted(row["id"] for row in projected)),
            "ownership_decisions_sha256": stable_digest(sorted(projected, key=lambda row: row["id"]))}
        write_json(inventory / "mobility-policy.json", mobility)
        script("check_session_mobility", catalog=catalogs["backend"], ownership=policies["backend"],
               mobility=inventory / "mobility-policy.json", guc_inventory=guc_inventory, guc_policy=guc_policy,
               runtime_source=root / "runtime/src/postgres_backend_runtime.c", report=reports / "mobility.json")
        script("generate_backend_state_runtime", catalog=catalogs["backend"], policy=policies["backend"],
               guc_inventory=guc_inventory, review_rules=rules_path, alignment=reports / "backend-ownership.json",
               api_header=root / "runtime/include/postgamma/backend_state_runtime.h", output_dir=runtime,
               stamp=runtime / ".postgamma-backend-state-runtime.json")
        script("check_frontend_tool_state_policy", catalog=catalogs["tool"], policy=policies["tool"],
               report=reports / "tool-ownership.json")
        script("generate_tool_state_runtime", catalog=catalogs["tool"], policy=policies["tool"],
               alignment=reports / "tool-ownership.json", api_header=root / "runtime/include/postgamma/tool_state_runtime.h",
               output_dir=runtime, stamp=runtime / ".postgamma-tool-state-runtime.json")
        backend_layout = read_json(runtime / ".postgamma-backend-state-runtime.json")
        tool_layout = read_json(runtime / ".postgamma-tool-state-runtime.json")
        report["state_layout"] = {"backend_bytes": backend_layout["owner_bytes"],
                                  "backend_slots": backend_layout["owner_slots"],
                                  "tool_bytes": tool_layout["byte_count"],
                                  "tool_alignment": tool_layout["alignment"],
                                  "tool_slots": tool_layout["slot_count"],
                                  "actual_linked_tls_measured": False,
                                  "actual_linked_static_measured": False}
        script("generate_backend_state_plan", catalog=catalogs["backend"], policy=policies["backend"],
               runtime=runtime / ".postgamma-backend-state-runtime.json", source_root=source, generated_root=build,
               output=plans / "backend.json", generated_output=plans / "backend-generated.json")
        script("generate_tool_state_plan", catalog=catalogs["tool"], policy=policies["tool"],
               runtime=runtime / ".postgamma-tool-state-runtime.json", source_root=source,
               embedded_adapter=root / "manifests/postgresql/embedded.json", output=plans / "tool.json")
        script("reconcile_context_state_plans", backend_plan=plans / "backend.json",
               tool_plan=plans / "tool.json", output=plans / "context.json")
        ast_cached([*ast, "--mode=plan", f"--output={plans / 'gucs.json'}",
                    f"--generated-output={plans / 'gucs-generated.json'}"],
                   [plans / 'gucs.json', plans / 'gucs-generated.json'], guc_inputs, log, env, root)
        script("generate_backend_quantum_plan", manifest=root / "manifests/postgresql/backend-quantum.json",
               source_root=source, output=plans / "quantum.json")
        script("merge_replacement_plans", plan=[plans / name for name in ("gucs.json", "context.json", "quantum.json")],
               output=plans / "postgres.json")
        script("merge_replacement_plans", plan=[plans / name for name in ("gucs-generated.json", "backend-generated.json")],
               output=plans / "postgres-generated.json")
        script("check_backend_execution_policy", catalog=inventory / "executions.json",
               policy=root / "manifests/execution/backend-types.json", output=reports / "execution-policy.json")
        script("generate_backend_execution_facts", alignment=reports / "execution-policy.json",
               output=runtime / "include/postgamma/backend_execution.inc")
        from run_backend_inheritance_inventory import command as inheritance_command
        from postgresql_adapter import load_adapter
        invocation = inheritance_command(host / "postgamma-inheritance-inventory", source, build,
                                         databases["backend"], inventory / "inheritance.json", load_adapter(adapter))
        run([*invocation, f"--clang-resource-dir={resource}"], log, env, root)
        script("generate_backend_inheritance", catalog=inventory / "inheritance.json",
               policy=root / "manifests/execution/backend-inheritance.json",
               runtime=runtime / ".postgamma-backend-state-runtime.json",
               report=reports / "inheritance.json", facts=runtime / "include/postgamma/backend_inheritance.inc")
        tree = directory / "generated/postgres"
        script("materialize", repository=root / "postgres", manifest=root / "manifests/upstream.json",
               destination=tree, build_root=config.build)
        apply_adaptations(tree, kernel_inputs["source_adaptations"])
        report["source_adaptations"] = kernel_inputs["source_adaptations"]
        script("apply_replacements", plan=plans / "postgres.json", tree=tree)
        script("install_generated_support", project_root=root, runtime_dir=runtime, tree=tree,
               adapter=adapter, profile="product", candidate_probe=candidate_probe)
        report["outputs"] = {str(path.relative_to(directory)): digest(path)
                             for subdir in (runtime, plans, tree)
                             for path in sorted(subdir.rglob("*")) if path.is_file()}
        verify_generation_sources(root, report["inputs"])
        report["status"] = "passed"
    except Exception as error:
        report["error"] = str(error)
        raise
    finally:
        write_json(current / "report.json", report)
        write_json(reports / "generate.json", {"status": report["status"], "run_report": str(current / "report.json")})
        print(f"wasm generation: {report['status']}; {current / 'report.json'}", flush=True)
    return directory


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    for option in ("build-dir", "deps-dir", "sdk-dir", "profile"):
        parser.add_argument("--" + option, type=Path)
    parser.add_argument("--target", default="wasm32-emscripten-pthreads")
    parser.add_argument("--mode", choices=("debug", "release"), default="debug")
    parser.add_argument("--longjmp", default="emscripten")
    parser.add_argument("--node", default="")
    parser.add_argument("--browser-executable", default="")
    parser.add_argument("--jobs", type=int, default=2)
    parser.add_argument("--candidate-probe", action="store_true")
    parser.add_argument("--check-tools", action="store_true")
    args = parser.parse_args()
    try:
        config = configuration(args)
        if args.check_tools: check_generator_tools(config)
        else: generate(config, args.longjmp, args.candidate_probe)
    except (WasmError, OSError, ValueError) as error:
        parser.exit(2, f"wasm generation: {error}\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
