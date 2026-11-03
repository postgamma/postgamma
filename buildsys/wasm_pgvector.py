"""Build the pinned static pgvector through the shared target state pipeline."""
from pathlib import Path
import shlex
import sys

from filter_compile_database import select_compile_records
from wasm_generate import ast_cached, tree_hashes
from wasm_toolchain import WasmError, capture, digest, read_json, run, write_json


def build(builder, module):
    root, env = builder.root, builder.env
    source = root / "third_party/pgvector"
    upstream = read_json(root / "manifests/extensions/pgvector-upstream.json")
    if (capture(["git", "-C", str(source), "rev-parse", "HEAD"]) != upstream["commit"] or
            capture(["git", "-C", str(source), "status", "--porcelain", "--untracked-files=all"]) or
            capture(["git", "-C", str(source), "describe", "--tags", "--exact-match", "HEAD"]) != upstream["ref_name"]):
        raise WasmError("pgvector source must be clean and match its pinned identity")
    from check_pgvector_contract import expected_symbols, control_value, validate_kernel_macro_bridges
    if module["version"] != upstream["version"] or control_value(
            (source / "vector.control").read_text(), "default_version") != upstream["version"]:
        raise WasmError("pgvector resource version differs from the static module")
    symbols = {entry["logical_name"]: entry["linker_name"] for entry in module["symbols"]}
    if symbols != expected_symbols((source / "sql/vector.sql").read_text(), source):
        raise WasmError("pgvector static symbol manifest does not match its SQL and worker entries")
    output = builder.output.parent / "pgvector"
    output.mkdir(parents=True, exist_ok=True)
    current = builder.current / "pgvector"
    current.mkdir(parents=True, exist_ok=True)
    database = output / "compile-db"
    fragments = current / "fragments"
    fragments.mkdir(parents=True)
    capture_env = dict(env, EM_COMPILER_WRAPPER=str(root / "buildsys/wasm_cc_capture.py"),
                       POSTGAMMA_CAPTURE_DIR=str(fragments))
    scalar = ["-DDISABLE_DISPATCH", "-mno-simd128", "-mno-relaxed-simd", "-fno-vectorize", "-fno-slp-vectorize"]
    adapter = ["-I" + str(root / "extensions/pgvector/include"),
               "-include", "postgamma_pgvector_adapter.h", "-D_PG_init=postgamma_pgvector_upstream_init"]
    sources = sorted((source / "src").glob("*.c"))
    for path in sources:
        run([builder.emcc, "-std=gnu11", *builder.flags, *scalar, *adapter,
             "-I" + str(source / "src"), "-c", str(path), "-o", str(output / (path.stem + ".o"))],
            builder.log, capture_env, output)
    rows = [read_json(p) for p in sorted(fragments.glob("*.json"))]
    if len(rows) != len(sources) or any("wasm32" not in " ".join(row["arguments"]) for row in rows):
        raise WasmError("pgvector capture must contain every actual wasm32 compilation")
    domain = root / "manifests/domains/pgvector.json"
    rows, selection = select_compile_records(rows, source, None, read_json(domain))
    if selection["selected_translation_units"] != len(sources):
        raise WasmError("pgvector target inventory omitted a translation unit")
    write_json(database / "compile_commands.json", rows)
    from wasm_kernel_checks import validate_database
    capture_report = validate_database(database / "compile_commands.json", root, builder.config.build,
        Path(builder.tools["settings"]["LLVM_ROOT"]) / "clang", Path(builder.tools["settings"]["CACHE"]) / "sysroot")
    selected = shlex.split(capture([sys.executable, str(root / "buildsys/list_compile_sources.py"),
        "--database", str(database / "compile_commands.json"), "--source-root", str(source),
        "--domain-manifest", str(domain), "--selection-report", str(current / "selection.json")], env))
    if set(selected) != {str(p) for p in sources}:
        raise WasmError("pgvector AST selection differs from the compiled source set")
    generation = read_json(Path(builder.report["generation_report"]))
    host = generation["host_tools"]["postgamma-state-inventory"]["path"]
    resource = generation["target_resource_directory"]
    catalog = output / "state.json"
    identity = {"database": digest(database / "compile_commands.json"),
                "sources": tree_hashes(source), "adapter": tree_hashes(root / "extensions/pgvector"),
                "generation": digest(Path(builder.report["generation_report"])), "tools": generation["host_tools"]}
    ast_cached([host, "--source-root=" + str(source), "--definition-domain=" + str(domain),
                "--clang-resource-dir=" + resource, "--output=" + str(catalog), "-p=" + str(database),
                *(str(p) for p in sources)], [catalog], identity, builder.log, env, root)
    runtime = builder.directory / "generated/runtime/.postgamma-backend-state-runtime.json"
    macro_bridges = validate_kernel_macro_bridges(source,
        root / "extensions/pgvector/include/postgamma_pgvector_adapter.h", read_json(runtime))
    command = [sys.executable, str(root / "buildsys/generate_extension_state_plan.py")]
    for key, value in {
        "catalog": catalog, "policy": root / module["state_policy"], "source-root": source,
        "selection": current / "selection.json", "backend-state-policy": builder.directory / "inventory/backend-policy.json",
        "backend-state-runtime": runtime, "guc-manifest": builder.directory / "inventory/guc-transform.json",
        "output": current / "plan.json", "report": current / "state.json",
    }.items():
        command += ["--" + key, str(value)]
    run(command, builder.log, env, root)
    generated = builder.output.parent / "generated/pgvector"
    run([sys.executable, str(root / "buildsys/materialize.py"), "--repository", str(source),
         "--manifest", str(root / "manifests/extensions/pgvector-upstream.json"),
         "--destination", str(generated), "--build-root", str(builder.config.build)], builder.log, env, root)
    run([sys.executable, str(root / "buildsys/apply_replacements.py"),
         "--plan", str(current / "plan.json"), "--tree", str(generated)], builder.log, env, root)
    jobs = [(generated / "src" / p.name, "modules/pgvector/" + p.stem,
             [*scalar, *adapter, "-I" + str(generated / "src")]) for p in sources]
    jobs.append((root / "extensions/pgvector/src/pgvector_adapter.c", "modules/pgvector/adapter",
                 [*scalar, "-I" + str(root / "extensions/pgvector/include")]))
    builder.namespace_group("pgvector", jobs, mapping=symbols)
    state = read_json(current / "state.json")
    builder.report["pgvector"] = {"upstream": upstream, "scalar_flags": scalar,
        "capture": capture_report,
        "compile_database": str(database / "compile_commands.json"), "compile_database_sha256": digest(database / "compile_commands.json"),
        "state_report": str(current / "state.json"), "state_summary": state["summary"],
        "plan_sha256": digest(current / "plan.json"), "catalog_sha256": digest(catalog),
        "generated": tree_hashes(generated), "kernel_macro_bridges": macro_bridges,
        "registered_symbols": len(symbols)}
