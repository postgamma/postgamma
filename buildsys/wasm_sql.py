#!/usr/bin/env python3
"""Build or exercise the browser SQL API and independent sessions."""

from __future__ import annotations

import argparse
from pathlib import Path
from threading import Event

from wasm import run_browser, serve
from wasm_link import Builder
from wasm_toolchain import WasmError, configuration, digest, read_json, run, write_json


def startup(config, longjmp: str, action: str, scenario: str) -> Path:
    builder = Builder(config, longjmp)
    report = {"kind": "postgamma.wasm-sql", "status": "failed",
              "sql_validated": False, "action": action, "scenario": scenario}
    try:
        builder.backend()
        builder.frontends()
        builder.modules()
        builder.product()
        builder.link(config.root / "wasm/bridge/sql.c")
        site = builder.output / "site"
        report["site"] = str(site)
        sources = [(str(p.relative_to(config.root)), "src/" + str(p.relative_to(config.root / "wasm/src").with_suffix(".js")))
                   for p in sorted((config.root / "wasm/src").rglob("*.ts"))]
        sources += [(str(p.relative_to(config.root)), "tests/" + p.with_suffix(".js").name)
                    for p in sorted((config.root / "tests/wasm/sql").glob("*.ts"))]
        for source, target in sources:
            run([builder.tools["node"], str(config.root / "buildsys/wasm_typescript.mjs"),
                 str(config.root / source), str(site / target)],
                builder.current / "typescript.log", builder.env, config.root)
        (site / "sql.html").write_bytes((config.root / "tests/wasm/sql/sql.html").read_bytes())
        report["link_report"] = str(builder.current / "report.json")
        report["harness"] = {name: digest(site / name)
                             for name in (*[target for _, target in sources], "sql.html")}
        if action == "check":
            if scenario in ("results", "budget"):
                from wasm_sql_oracle import oracle
                report["oracle"] = oracle(config.root, builder.current, site)
            run([builder.tools["node"], str(config.root / "tests/wasm/sql/protocol.mjs"),
                 str(site / "src/protocol/wire.js")], builder.current / "protocol.log",
                builder.env, config.root)
            with serve(site, "sql.html") as url:
                report["browser"] = run_browser(config, builder.tools, builder.env, builder.current, url + "?scenario=" + scenario)
            probe = report["browser"]["probe"]
            if probe.get("scenario") != scenario or not probe.get("checks") or probe.get("pageTicks", 0) < 2:
                raise WasmError("incomplete browser SQL evidence")
            report["sql_validated"] = True
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
    parser.add_argument("--scenario", default="abi")
    args = parser.parse_args()
    try:
        path = startup(configuration(args), args.longjmp, args.action, args.scenario)
        if args.action == "serve":
            with serve(Path(read_json(path)["site"]), "sql.html") as url:
                print(f"SQL API fixture: {url} (Ctrl-C to stop)", flush=True)
                Event().wait()
    except KeyboardInterrupt:
        pass
    except Exception as error:
        parser.exit(2, "wasm SQL: " + str(error).splitlines()[0] + "\n")


if __name__ == "__main__":
    main()
