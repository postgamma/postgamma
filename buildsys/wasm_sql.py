#!/usr/bin/env python3
"""Build or exercise the browser SQL API and independent sessions."""

from __future__ import annotations

import argparse
from pathlib import Path
from threading import Event

from wasm import run_browser, serve
from wasm_link import Builder
from wasm_toolchain import WasmError, configuration, digest, read_json, run, write_json


def startup(config, longjmp: str, action: str, scenario: str, extensions=()) -> Path:
    builder = Builder(config, longjmp, ("pgvector",) if scenario.startswith("vector") else extensions)
    report = {"kind": "postgamma.wasm-sql", "status": "failed",
              "sql_validated": False, "action": action, "scenario": scenario}
    try:
        from wasm_sql_budget import SCENARIOS, generate_policy, summarize
        policy = read_json(config.root / "manifests/profiles/wasm-resource-budget.json")
        generate_policy(config.root, builder.output, policy)
        builder.backend()
        builder.frontends()
        builder.modules()
        builder.product()
        builder.link(config.root / "wasm/bridge/sql.c",
                     ("-DPOSTGAMMA_WASM_TESTING",) if action == "check" else ())
        site = builder.output / "site"
        report["site"] = str(site)
        sources = [(str(p.relative_to(config.root)), "src/" + str(p.relative_to(config.root / "wasm/src").with_suffix(".js")))
                   for p in sorted((config.root / "wasm/src").rglob("*.ts")) if not p.name.endswith(".d.ts")]
        sources += [(str(p.relative_to(config.root)), "tests/" + p.with_suffix(".js").name)
                    for p in sorted((config.root / "tests/wasm/sql").glob("*.ts"))]
        sources += [(str(p.relative_to(config.root)), "storage-tests/" + p.with_suffix(".js").name)
                    for p in sorted((config.root / "tests/wasm/storage").glob("*.ts"))]
        sources += [(str(p.relative_to(config.root)), "vector-tests/" + p.with_suffix(".js").name)
                    for p in sorted((config.root / "tests/wasm/vector").glob("*.ts"))]
        sources.append(("examples/wasm/vector-search.ts", "vector-example.js"))
        report["harness_sources"] = {source: digest(config.root / source) for source, _ in sources}
        for source, target in sources:
            run([builder.tools["node"], str(config.root / "buildsys/wasm_typescript.mjs"),
                 str(config.root / source), str(site / target)],
                builder.current / "typescript.log", builder.env, config.root)
        (site / "sql.html").write_bytes((config.root / "tests/wasm/sql/sql.html").read_bytes())
        (site / "recovery.html").write_bytes((config.root / "tests/wasm/storage/recovery.html").read_bytes())
        (site / "vector-search.html").write_bytes((config.root / "examples/wasm/vector-search.html").read_bytes())
        from wasm_assets import generate as asset_manifest
        report["asset_manifest"] = asset_manifest(builder)
        report["harness_sources"].update({name: digest(config.root / name) for name in
            ("tests/wasm/storage/recovery.html", "tests/wasm/storage/recovery.mjs", "examples/wasm/vector-search.html")})
        report["link_report"] = str(builder.current / "report.json")
        report["harness"] = {name: digest(site / name)
                             for name in (*[target for _, target in sources], "sql.html", "recovery.html", "vector-search.html", "src/protocol/limits.js", "src/loader/identity.js", "postgamma.assets.json")}
        if action == "check":
            if scenario.startswith("vector"):
                check_source = "tests/wasm/vector/budget.mjs"
                report["harness_sources"][check_source] = digest(config.root / check_source)
                run([builder.tools["node"], str(config.root / check_source), str(site / "vector-tests/budget.js")],
                    builder.current / "vector-budget-test.log", builder.env, config.root)
                from wasm_pgvector_fixtures import fixtures
                report["vector_fixtures"] = fixtures(config.root, builder.current, site)
                report["harness_sources"].update(report["vector_fixtures"]["inputs"])
                report["harness"]["vector-fixtures.json"] = digest(site / "vector-fixtures.json")
            if scenario == "vector-all":
                from wasm_vector_budget import baseline
                vector_baseline = baseline(builder)
            if scenario in ("results", "all"):
                from wasm_sql_oracle import oracle
                report["oracle"] = oracle(config.root, builder.current, site, config.build)
                report["harness"].update({name: digest(site / name) for name in ("fixtures.json", "oracle.json")})
            run([builder.tools["node"], str(config.root / "tests/wasm/sql/protocol.mjs"),
                 str(site / "src/protocol/wire.js")], builder.current / "protocol.log",
                builder.env, config.root)
            run([builder.tools["node"], str(config.root / "tests/wasm/storage/namespace.mjs"),
                 str(site / "src/storage/namespace.js")], builder.current / "namespace.log", builder.env, config.root)
            scenarios = SCENARIOS if scenario == "all" else ("storage", "durability", "storage-errors", "recovery") if scenario == "storage-all" else (scenario,)
            if scenario == "vector-all":
                scenarios = ("abi", "vector-load", "vector-query", "vector-storage", "vector-example")
            report["browsers"] = {}
            with serve(site, "sql.html") as url:
                for name in scenarios:
                    print("wasm SQL: checking " + name, flush=True)
                    if name == "recovery":
                        from wasm_storage_recovery import check
                        report["recovery"] = check(config, builder, url.replace("sql.html", "recovery.html"))
                        continue
                    timeout = 120000 if name in ("vector-query", "vector-storage", "vector-example") else 90000 if name in ("storage", "durability", "storage-errors") else 30000
                    browser = run_browser(config, builder.tools, builder.env, builder.current / "cases" / name,
                                          url + "?scenario=" + name + ("&extension=pgvector" if builder.extensions else ""), timeout_ms=timeout)
                    report["browsers"][name] = browser
                    probe = browser["probe"]
                    if probe.get("scenario") != name or not probe.get("checks") or probe.get("pageTicks", 0) < 2:
                        raise WasmError("incomplete browser SQL evidence")
            if scenario == "vector-all":
                from wasm_vector_budget import summarize as vector_summary
                report["vector_resource_budget"] = vector_summary(builder, vector_baseline, report["browsers"])
                write_json(builder.current / "vector-resource-budget.json", report["vector_resource_budget"])
            if scenario == "all":
                report["resource_budget"] = summarize(policy, builder.report["audit"]["layout"], report["browsers"], config.mode)
                write_json(builder.current / "sql-resource-budget.json", report["resource_budget"])
            report["sql_validated"] = True
        for source, checksum in report["harness_sources"].items():
            if digest(config.root / source) != checksum:
                raise WasmError("SQL harness source changed during validation: " + source)
        for source, checksum in report["harness"].items():
            if digest(site / source) != checksum:
                raise WasmError("SQL harness asset changed during validation: " + source)
        if builder.snapshot() != builder.source_snapshot or digest(config.root / "wasm/bridge/sql.c") != builder.report["bridge"]["sha256"]:
            raise WasmError("kernel inputs changed during SQL validation")
        report["status"] = "passed"
    except Exception as error:
        report["error"] = str(error)
        raise
    finally:
        builder.finish()
        path = builder.current / "sql.json"
        write_json(path, report)
        write_json(builder.directory / "reports/sql.json", {
            "status": report["status"], "sql_validated": report["sql_validated"],
            "run_report": str(path)})
        print(f"wasm SQL: {report['status']}; {path}", flush=True)
    return path


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("build", "check", "serve"))
    parser.add_argument("--root", required=True, type=Path)
    for name in ("build-dir", "deps-dir", "sdk-dir", "profile"):
        parser.add_argument("--" + name, type=Path)
    parser.add_argument("--target", default="wasm32-emscripten-pthreads")
    parser.add_argument("--mode", default="debug", choices=("debug", "release"))
    parser.add_argument("--longjmp", default="emscripten")
    parser.add_argument("--node", default="")
    parser.add_argument("--browser-executable", default="")
    parser.add_argument("--jobs", type=int, default=2)
    parser.add_argument("--scenario", default="all")
    parser.add_argument("--extensions", choices=("none", "pgvector"), default="none")
    args = parser.parse_args()
    try:
        path = startup(configuration(args), args.longjmp, args.action, args.scenario,
                       () if args.extensions == "none" else (args.extensions,))
        if args.action == "serve":
            with serve(Path(read_json(path)["site"]), "vector-search.html" if args.scenario == "vector-example" else "sql.html") as url:
                print(f"SQL API fixture: {url} (Ctrl-C to stop)", flush=True)
                Event().wait()
    except KeyboardInterrupt:
        pass
    except Exception as error:
        parser.exit(2, "wasm SQL: " + str(error).splitlines()[0] + "\n")


if __name__ == "__main__":
    main()
