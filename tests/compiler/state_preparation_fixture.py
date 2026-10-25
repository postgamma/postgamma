"""Check initializer facts against target ABIs, without changing accessors."""

from __future__ import annotations

import json
from pathlib import Path


def check(run, tool: Path, fixtures: Path, build: Path) -> None:
    domain = build / "state-initializers-domain.json"
    domain.write_text(json.dumps({
        "schema_version": 1, "kind": "postgamma.postgres-source-domain",
        "translation_unit_files": ["state_initializers.c", "state_initializers.cpp"],
    }) + "\n", encoding="utf-8")
    for target in ("x86_64-unknown-linux-gnu", "aarch64-unknown-linux-gnu",
                   "wasm32-unknown-unknown", "powerpc64-unknown-linux-gnu"):
        output = build / f"initializers-{target}.json"
        command = [str(tool), f"--source-root={fixtures}",
                   f"--definition-domain={domain}", f"--output={output}",
                   str(fixtures / "state_initializers.c"), "--", "-std=c11",
                   f"--target={target}"]
        run(command, cwd=fixtures)
        first = output.read_bytes()
        run(command, cwd=fixtures)
        if first != output.read_bytes():
            raise RuntimeError("initializer facts are not deterministic")
        candidates = {c["name"]: c for c in json.loads(first)["candidates"]}
        proven = {name for name, c in candidates.items() if c["preparation"]["zero_bytes"]}
        expected = {"explicit_zero", "boolean_zero", "pointer_zero", "float_zero",
                    "array_zero", "function_zero", "weak_zero", "asm_zero"}
        # x86-64 long double has padding. Other targets also retain the
        # conservative 32/64-bit floating-representation restriction.
        if target.startswith("powerpc64"):
            expected = set()
        if proven != expected:
            raise RuntimeError(f"{target}: zero image proof differs: {proven ^ expected}")
        for name in ("negative_zero", "nonzero", "internal_template", "record_zero"):
            if not candidates[name]["preparation"]["constant_evaluated"]:
                raise RuntimeError(f"{target}: constant evaluation failed for {name}")
        for name in ("weak_zero", "asm_zero"):
            if candidates[name]["preparation"]["ordinary_symbol"]:
                raise RuntimeError(f"{name}: nonordinary linkage was lost")
        if candidates["implicit_zero"]["preparation"]["zero_reason"] != "implicit_zero_storage":
            raise RuntimeError("implicit initialization was confused with evaluated zero")

    audit_root = build / "preparation-audit"
    audit_root.mkdir(exist_ok=True)
    audit_source = audit_root / "audit.c"
    audit_source.write_text("""
extern void *postgamma_backend_state_address_cached(int, const volatile void *);
extern void *postgamma_backend_state_direct_address(int, const volatile void *);
extern void consume(const void *);
static int state = 42;
static int *pointer = &state;
int good(void) { return *(__typeof__(&(state)))postgamma_backend_state_address_cached(7, &(state)); }
int direct(void) { return *(__typeof__(&(state)))postgamma_backend_state_direct_address(7, &(state)); }
const volatile void *provider(void) { return &(state); }
void write_template(void) { state++; }
int read_template(void) { return state; }
void escape(void) { consume(&state); }
int wrong_operand(void) { return *(__typeof__(&(state)))postgamma_backend_state_address_cached(7, &state + 1); }
void asm_symbol(void) { __asm__ volatile("mov state, %%eax" ::: "eax"); }
void asm_wait(void) { __asm__ volatile(" rep; nop\\t\\n"); }
void asm_empty(void) { __asm__ volatile(""); }
void asm_unknown(void) { __asm__ volatile("repnop"); }
void asm_directive(void) { __asm__ volatile(".long state"); }
""", encoding="utf-8")
    audit_domain = audit_root / "domain.json"
    audit_domain.write_text(json.dumps({
        "schema_version": 1, "kind": "postgamma.postgres-source-domain",
        "translation_unit_files": ["audit.c"],
    }), encoding="utf-8")
    output = audit_root / "audit.json"
    run([str(tool), "--audit-preparation", f"--source-root={audit_root}",
         f"--definition-domain={audit_domain}", f"--output={output}",
         str(audit_source), "--", "-std=c11"], cwd=audit_root)
    audit = json.loads(output.read_text())
    kinds = [u["template_use"]["kind"] for u in audit["uses"]]
    if kinds.count("unconverted") != 4 or kinds.count("resolver_template") != 2:
        raise RuntimeError(f"template audit accepted a raw use/escape: {kinds}")
    if kinds.count("unevaluated") != 3 or kinds.count("provider_return") != 1:
        raise RuntimeError("template audit did not distinguish type and provider uses")
    if not audit["preparation_audit"]["opaque_references"]:
        raise RuntimeError("assembly symbol references were not recorded")
    assembly = {r["text"].strip(): r["unbounded"]
                for r in audit["preparation_audit"]["opaque_references"]}
    if assembly.get("rep; nop") is not False or assembly.get("") is not False:
        raise RuntimeError("operand-free x86 wait instructions remained opaque")
    if assembly.get("repnop") is not True or assembly.get(".long state") is not True:
        raise RuntimeError("unknown assembly obtained a no-symbol proof")
    if str(audit_source) not in audit["preparation_audit"]["inputs"]:
        raise RuntimeError("audit lacks source input identity")
    wait_source = audit_root / "wait.c"
    wait_source.write_text('void wait_hint(void) { __asm__("rep; nop"); }\n')
    wait_domain = audit_root / "wait-domain.json"
    wait_domain.write_text(json.dumps({
        "schema_version": 1, "kind": "postgamma.postgres-source-domain",
        "translation_unit_files": ["wait.c"],
    }))
    for target, unbounded in (("x86_64-unknown-linux-gnu", False),
                              ("aarch64-unknown-linux-gnu", True)):
        run([str(tool), "--audit-preparation", f"--source-root={audit_root}",
             f"--definition-domain={wait_domain}", f"--output={output}",
             str(wait_source), "--", "-std=c11", f"--target={target}"], cwd=audit_root)
        references = json.loads(output.read_text())["preparation_audit"]["opaque_references"]
        if len(references) != 1 or references[0]["unbounded"] is not unbounded:
            raise RuntimeError(f"assembly proof ignored target architecture: {target}")
    cpp = audit_root / "headers.cpp"
    cpp.write_text("#include <cstdlib>\nstatic int state;\nint f() { return state; }\n")
    audit_domain.write_text(json.dumps({
        "schema_version": 1, "kind": "postgamma.postgres-source-domain",
        "translation_unit_files": ["headers.cpp"],
    }))
    run([str(tool), "--audit-preparation", f"--source-root={audit_root}",
         f"--definition-domain={audit_domain}", f"--output={output}",
         str(cpp), "--", "-std=c++17"], cwd=fixtures)
    import hashlib
    for path, expected in json.loads(output.read_text())["preparation_audit"]["inputs"].items():
        if hashlib.sha256(Path(path).read_bytes()).hexdigest() != expected:
            raise RuntimeError(f"audit resolved an include against the wrong cwd: {path}")

    output = build / "initializers-cxx.json"
    run([str(tool), f"--source-root={fixtures}", f"--definition-domain={domain}",
         f"--output={output}", str(fixtures / "state_initializers.cpp"),
         "--", "-std=c++17"], cwd=fixtures)
    candidates = {c["name"]: c for c in json.loads(output.read_text())["candidates"]}
    if any(c["preparation"]["zero_bytes"] for c in candidates.values()):
        raise RuntimeError("dynamic initialization/member pointers obtained zero image proof")
    if candidates["dynamic_initializer"]["preparation"]["zero_reason"] != "nonconstant_initializer":
        raise RuntimeError("dynamic initialization was not distinguished")
