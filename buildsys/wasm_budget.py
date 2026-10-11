#!/usr/bin/env python3
"""Measure a provisional resource envelope and collect fresh platform evidence."""

from pathlib import Path

from wasm_toolchain import WasmError, digest, read_json, run_id, write_json


POLICY = "manifests/profiles/wasm-resource-budget.json"


def plan(policy: dict) -> dict:
    if policy.get("status") not in ("provisional", "validated") or policy.get("target") != "wasm32-emscripten-pthreads":
        raise WasmError("resource probe requires a reviewed wasm32 policy")
    roles = policy["thread_roles"]
    if len(roles) != 6 or {r["role"] for r in roles} != {"supervisor", "background", "carrier", "io_owner", "logical_tool", "reserve"}:
        raise WasmError("resource policy must account for every pthread role")
    if any(type(r["count"]) is not int or not 1 <= r["count"] <= 32 for r in roles):
        raise WasmError("resource probe requires bounded positive integer role counts")
    stacks = [r["stack_bytes"] for r in roles for _ in range(r["count"])]
    amounts = [policy[k] for k in ("initial_memory_bytes", "maximum_memory_bytes", "control_stack_bytes",
                                  "static_reserve_bytes", "tls_reserve_per_worker_bytes")]
    amounts += stacks + [r["bytes"] for r in policy["linear_allocations"]]
    if any(type(n) is not int or n <= 0 or n % 65536 for n in amounts):
        raise WasmError("resource sizes must be positive Wasm page multiples")
    if not 2 <= len(stacks) <= 32:
        raise WasmError("resource probe requires 2..32 bounded pthreads")
    carriers = next(r["count"] for r in roles if r["role"] == "carrier")
    if policy["initial_max_live_sessions"] != carriers:
        raise WasmError("initial live-session budget must match the carrier envelope")
    workers = len(stacks) + 1
    stack_bytes = sum(stacks) + policy["control_stack_bytes"]
    tls_bytes = workers * policy["tls_reserve_per_worker_bytes"]
    allocation_bytes = sum(r["bytes"] for r in policy["linear_allocations"])
    total = stack_bytes + tls_bytes + policy["static_reserve_bytes"] + allocation_bytes
    maximum = policy["maximum_memory_bytes"]
    if not policy["initial_memory_bytes"] < total < maximum <= 2**31:
        raise WasmError("resource probe requires growth to an envelope below the wasm32 candidate maximum")
    return {"pthread_stacks": stacks, "pthread_count": len(stacks), "worker_count": workers,
            "stack_bytes": stack_bytes, "tls_reserve_bytes": tls_bytes,
            "allocation_bytes": allocation_bytes, "estimated_linear_peak_bytes": total,
            "maximum_headroom_bytes": maximum - total}


def probe_configuration(root: Path) -> tuple[str, list[str]]:
    policy = read_json(root / POLICY)
    budget = plan(policy)
    header = "/* Generated from the provisional resource policy, for probes only. */\n"
    values = {"RESOURCE_THREADS": budget["pthread_count"], "RESOURCE_TLS": policy["tls_reserve_per_worker_bytes"],
              "RESOURCE_STATIC": policy["static_reserve_bytes"], "RESOURCE_ALLOCATION": budget["allocation_bytes"],
              "RESOURCE_MAXIMUM": policy["maximum_memory_bytes"]}
    header += "".join(f"#define {name} {value}U\n" for name, value in values.items())
    header += "static const size_t resource_stacks[] = {" + ",".join(map(str, budget["pthread_stacks"])) + "};\n"
    flags = [f"-sPTHREAD_POOL_SIZE={budget['pthread_count']}",
             f"-sINITIAL_MEMORY={policy['initial_memory_bytes']}", f"-sMAXIMUM_MEMORY={policy['maximum_memory_bytes']}",
             f"-sSTACK_SIZE={policy['control_stack_bytes']}", "-sABORTING_MALLOC=0", "-sSTACK_OVERFLOW_CHECK=2",
             '-sEXPORTED_RUNTIME_METHODS=["HEAPU8","UTF8ToString"]']
    return header, flags


def summarize(policy: dict, evidence: dict) -> dict:
    budget = plan(policy)
    for name in ("platform", "wait", "recovery", "opfs", "deployment", "resource"):
        item = evidence.get(name, {})
        if item.get("status") != "passed" or item.get("browser_probe_validated") is not True:
            raise WasmError(f"resource decision requires a fresh passing {name} browser report")
    platform = evidence["platform"]["browser"]["probe"]
    resource = evidence["resource"]["browser"]["probe"]
    if platform["baseline"]["abi"]["pointer"] != 4:
        raise WasmError("resource ABI differs from the wasm32 candidate")
    if resource["budget"]["pthreadCount"] != budget["pthread_count"] or not resource["growth"]["allocationRejected"]:
        raise WasmError("resource evidence does not cover the planned capacity and allocation limit")
    if policy["memory64_cost_hypothesis"] is not None:
        raise WasmError("a Memory64 cost hypothesis requires a separate target-width review")
    opfs = evidence["opfs"]["browser"]["probe"]
    return {
        "plan": budget, "policy": policy,
        "measured_probe": resource,
        "measured_abi": platform["baseline"]["abi"],
        "measured_small_stacks": platform["rounds"][0]["threads"],
        "kernel_validated": False, "kernel_resource_policy_frozen": False,
        "decision": {
            "first_kernel_target": policy["target"], "emscripten_longjmp": "emscripten",
            "thread_model": "control Worker, supervisor, bounded background roles and independent carriers",
            "first_storage_profile": policy["first_storage_profile"], "opfs_durability_validated": False,
            "memory64_triggered": False,
            "reason": "The scoped wasm32 ABI, thread-capacity and allocation probes pass within the provisional envelope; no specific Memory64 adaptation-cost hypothesis is recorded.",
            "reopen_if": ["target-generated static or TLS layout exceeds reserves", "kernel memory demand exceeds the bounded envelope", "a concrete target-width adaptation cost is identified"],
        },
        "required_adaptations": [
            {"task": "W10/W11", "item": "Cross-configure and regenerate target state layouts; native sizes are not target sizes."},
            {"task": "W12", "item": "Use process-private synchronization, logical signal masks, monotonic deadlines, measured stack bounds and a deliberate locale provider."},
            {"task": "W12/W21/W22", "item": "Bound role launch and carrier admission, keep the control event loop live, and validate independent SQL sessions."},
            {"task": "W14/W25", "item": "Measure actual initdb, C stack depth, static/TLS state, memory files, query heaps, queues and JS memory before freezing limits."},
            {"task": "W30/W31", "item": "Resolve observed OPFS namespace, open-handle lifetime, flush-order and quota-error gaps before durable claims."},
        ],
        "opfs_scope": {"posix_storage_complete": opfs.get("posixStorageComplete", False),
                       "report_required_for_durable_profile": True},
        "accounting": {
            "linear_memory": "Stack, static, TLS, buffers, heap and queues are components of the same shared linear memory, counted once across Workers.",
            "outside_linear_memory": "The JS/VM/code estimate is separate and unmeasured; it is not added to Wasm allocation counts or presented as RSS.",
        "native": "Native budgets and historical measurements are inputs only. Per-cycle thread starts are not simultaneous residency.",
        },
    }


def check(config, longjmp: str) -> Path:
    from wasm import build_configuration, probe, source_identity
    from wasm_toolchain import fingerprint, sdk_tools

    if longjmp != "emscripten":
        raise WasmError("resource decisions use the selected emscripten longjmp mode; use recovery probes for alternatives")
    policy = read_json(config.root / POLICY)
    plan(policy)
    tools, _ = sdk_tools(config)
    directory = config.build / "wasm" / config.target / config.mode / fingerprint(build_configuration(config, tools))
    run = directory / "runs" / ("resource-budget-" + run_id())
    output = run / "report.json"
    report = {"kind": "postgamma.wasm-resource-budget", "schema_version": 1, "status": "failed",
              "kernel_validated": False, "policy_sha256": digest(config.root / POLICY),
              "source": source_identity(config), "evidence": {}}
    try:
        documents = {}
        for name in ("platform", "wait", "recovery", "opfs", "deployment", "resource"):
            print(f"wasm resource budget: checking {name}", flush=True)
            path = probe(config, check=True, name=name)
            documents[name] = read_json(path)
            report["evidence"][name] = {"path": str(path), "sha256": digest(path)}
        report.update(summarize(policy, documents))
        for item in documents.values():
            for path, checksum in item["inputs"]["source"]["sources"].items():
                if digest(config.root / path) != checksum:
                    raise WasmError(f"platform source changed during the resource run: {path}")
        native = config.root / "manifests/profiles/embedded-resource-budget.json"
        report["native_input"] = {"path": str(native), "sha256": digest(native), "policy": read_json(native)}
        historical = config.build / "embedded-kernel/reports/kernel-lifecycle-run.json"
        if historical.is_file():
            old = read_json(historical)
            report["native_historical_evidence"] = {"path": str(historical), "sha256": digest(historical),
                "not_rerun": True, "not_target_measurement": True,
                "resource_budget": old.get("resource_budget"), "stack_contract": old.get("stack_contract")}
        report["status"] = "passed"
    except Exception as error:
        report["error"] = str(error)
        raise
    finally:
        write_json(output, report)
        write_json(directory / "reports/resource-budget.json", dict(report, run_report=str(output)))
        print(f"wasm resource budget: {report['status']}; report: {output}", flush=True)
    return output
