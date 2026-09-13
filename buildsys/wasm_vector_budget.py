"""Compare pgvector resources against a verified extension-free kernel receipt."""
from pathlib import Path
from wasm_toolchain import WasmError, digest, read_json


def baseline(builder):
    site = builder.directory / "link/site"
    names = ("postgamma.wasm", "postgamma.data", "postgamma.mjs")
    for path in reversed(sorted((builder.directory / "runs").glob("link-*/sql.json"))):
        sql = read_json(path)
        if sql.get("status") != "passed" or sql.get("scenario") != "all":
            continue
        budget = sql.get("resource_budget", {})
        if budget.get("status") != "passed" or budget.get("validated_scope", {}).get("mode") != builder.config.mode:
            continue
        link_path = Path(sql["link_report"])
        if not link_path.is_file(): continue
        link = read_json(link_path)
        if link.get("extensions") or link.get("status") != "passed": continue
        if any(link["source_snapshot"][name] != builder.source_snapshot[name] for name in ("runtime", "embedded-c", "manifests")):
            continue
        if link["source_snapshot"]["buildsys"].get("buildsys/wasm_link.py") != builder.source_snapshot["buildsys"]["buildsys/wasm_link.py"]:
            continue
        if link["inputs"] != builder.inputs or link["bridge"] != builder.report["bridge"]:
            continue
        if any(not (site / name).is_file() or digest(site / name) != link["outputs"].get(name) for name in names):
            continue
        return {"receipt": str(path), "receipt_sha256": digest(path), "link_sha256": digest(link_path),
                "layout": link["audit"]["layout"], "artifact_bytes": {name: (site / name).stat().st_size for name in names},
                "artifact_sha256": {name: digest(site / name) for name in names},
                "abi_peak_linear_memory": max(m["peakMemory"] for m in sql["browsers"]["abi"]["probe"]["metrics"])}
    raise WasmError("vector budget needs matching base artifacts and a passed make wasm-sql-check receipt")


def summarize(builder, base, browsers):
    names = ("abi", "vector-load", "vector-query", "vector-storage", "vector-example")
    if tuple(browsers) != names or any(browsers[n]["status"] != "passed" for n in names):
        raise WasmError("vector acceptance requires every workload group")
    groups = {name: browsers[name]["probe"]["resourceBudget"] for name in names}
    layout = builder.report["audit"]["layout"]
    files = {name: (builder.output / "site" / name).stat().st_size for name in base["artifact_bytes"]}
    return {"kind": "postgamma.wasm-vector-resource-budget", "status": "passed",
            "scope": {"mode": builder.config.mode, "browser": "chromium", "pgvector": "0.8.6", "simd": False},
            "baseline": base, "layout": layout, "artifact_bytes": files,
            "artifact_byte_delta": {name: size - base["artifact_bytes"][name] for name, size in files.items()},
            "static_byte_delta": layout["__data_end"] - base["layout"]["__data_end"],
            "tls_byte_delta": layout["__tls_size"] - base["layout"]["__tls_size"],
            "observed_abi_linear_memory_delta": groups["abi"]["peakLinearMemory"] - base["abi_peak_linear_memory"],
            "groups": groups, "outside_linear_memory_rss_measured": False,
            "interpretation": "Artifact deltas compare matched core sources; the ABI case is repeated with the extension linked. Other workload peaks are reported separately and are not universal extension overhead."}
