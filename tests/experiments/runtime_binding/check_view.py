#!/usr/bin/env python3
"""Compare the explicit-view diagnostic with unchanged ABI call sites."""
import argparse
import json
import os
from pathlib import Path
import shlex
import signal
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tool", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--clang-resource-dir", type=Path, required=True)
    parser.add_argument("--cc", default=os.environ.get("CC", "cc"))
    parser.add_argument("--validated-entry", action="store_true")
    parser.add_argument("--clone-safe-only", action="store_true")
    parser.add_argument("--borrowed-scope", action="store_true")
    parser.add_argument("--inline-headers", action="store_true")
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    source = Path(__file__).with_name("view_fixture.c")
    candidate = args.output / "view.c"
    facts = args.output / "view.json"
    subprocess.run([str(args.tool.resolve()), str(source), str(candidate), str(facts),
                    *(["--validated-entry"] if args.validated_entry else []),
                    *(["--clone-safe-only"] if args.clone_safe_only else []),
                    *(["--borrowed-scope"] if args.borrowed_scope else []),
                    *(["--inline-headers"] if args.inline_headers else []),
                    "-std=c11", "-resource-dir=" + str(args.clang_resource_dir)], check=True)
    report = json.loads(facts.read_text())
    if args.borrowed_scope:
        selected = {row["function"] for row in report["functions"]}
        if not {"nested_macro_address", "nested_macro_callback", "read_binding_wrapper"} <= selected:
            raise RuntimeError("nested state macro accesses were not transformed")
        if "macro_binding_boundary" not in report["excluded_binding_boundaries"]:
            raise RuntimeError("a binding change hidden in a macro was borrowed")
        if "binding_wrapper" not in report["excluded_binding_boundaries"]:
            raise RuntimeError("a wrapper hid a known binding boundary")
    if args.inline_headers:
        headers = {row["function"] for row in report["header_functions"]}
        if headers != {"inline_state_address", "inline_state_read", "inline_main_read"}:
            raise RuntimeError("inline state closure or static object exclusion changed")
        if not {"inline_callback", "inline_address"} <= selected:
            raise RuntimeError("callers did not pass their view to inline helpers")
        if "late_state_read" not in report["excluded_late_headers"]:
            raise RuntimeError("a late header was moved before its types are visible")
    callback = "bind_then_read" if args.borrowed_scope else "callback_api"
    if not any(row["function"] == callback and row["indirect_calls"] == 1
               for row in report["functions"]):
        raise RuntimeError("unqualified callback edge was lost from the report")
    if args.validated_entry or args.clone_safe_only:
        excluded = {row["function"] for row in report["excluded_body_identity"]}
        if not {"static_identity", "string_identity", "compound_identity", "function_identity"} <= excluded:
            raise RuntimeError("a body with observable object identity was cloned")
    outputs = []
    for name, input_file in (("baseline", source), ("view", candidate)):
        executable = args.output / name
        command = [*shlex.split(args.cc), "-std=c11", "-O2", "-DNDEBUG", "-Wall", "-Wextra", "-Werror",
                   "-iquote", str(source.parent), str(input_file), "-o", str(executable)]
        subprocess.run(command, check=True)
        modes = [[], ["unbound-address"], ["unbound-nested-address"], ["unbound-inline-address"]]
        if args.validated_entry or args.borrowed_scope:
            modes.append(["unbound-callback"])
        if args.validated_entry or args.clone_safe_only or args.borrowed_scope:
            modes.append(["identities"])
        results = []
        for mode in modes:
            result = subprocess.run([str(executable.resolve()), *mode], timeout=10,
                                    stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            expected = -signal.SIGABRT if mode and mode[0].endswith("-address") else 0
            if result.returncode != expected or result.stderr:
                raise RuntimeError(f"{name} {mode}: exit {result.returncode}, expected {expected}: "
                                   + result.stderr.decode(errors="replace"))
            results.append((result.returncode, result.stdout))
        outputs.append(results)
    if outputs[0] != outputs[1]:
        raise RuntimeError("explicit-view output differs from the baseline")
    for _, stdout in outputs[1]:
        print(stdout.decode(), end="")


if __name__ == "__main__":
    main()
