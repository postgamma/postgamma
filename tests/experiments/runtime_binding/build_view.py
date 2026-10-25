#!/usr/bin/env python3
"""Build an isolated explicit-view diagnostic from captured compile/link records.

The input source, objects and installation are never overwritten. This is not
a production qualification policy: external/indirect callees must preserve the
logical binding when they return. The generated report retains those edges.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("compile-record", "link-record", "baseline-prefix", "tool", "output"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--clang-resource-dir", type=Path, required=True)
    parser.add_argument("--validated-entry", action="store_true")
    parser.add_argument("--clone-safe-only", action="store_true")
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    root = Path(__file__).resolve().parents[3]
    compile_record = json.loads(args.compile_record.read_text())
    link = json.loads(args.link_record.read_text())
    source = Path(compile_record["file"])
    report = {"status": "running", "scope": "diagnostic-only explicit view propagation",
              "source": str(source), "source_sha256": digest(source), "commands": [],
              "validated_entry": args.validated_entry,
              "clone_safe_only": args.clone_safe_only or args.validated_entry,
              "transform_tool_sha256": digest(args.tool),
              "baseline_sha256": digest(args.baseline_prefix / "bin/postgres")}

    def run(name, command, cwd):
        report["commands"].append({"name": name, "arguments": command, "directory": str(cwd)})
        with (out / (name + ".log")).open("w") as log:
            subprocess.run(command, cwd=cwd, stdout=log, stderr=subprocess.STDOUT,
                           check=True, timeout=300)

    try:
        command = link["arguments"].copy()
        command[command.index("-o") + 1] = str(out / "baseline-postgres")
        run("baseline-link", command, link["directory"])
        if digest(out / "baseline-postgres") != report["baseline_sha256"]:
            raise RuntimeError("captured link no longer reproduces the baseline")
        include = out / "include/postgamma"
        include.mkdir(parents=True)
        shutil.copy2(root / "runtime/include/postgamma/backend_state_runtime.h", include)
        report["view_header_sha256"] = digest(include / "backend_state_runtime.h")
        flags = []
        original = compile_record["arguments"]
        skip = False
        for arg in original[1:]:
            if skip:
                skip = False
                continue
            if arg == "-o":
                skip = True
                continue
            if arg == "-c" or arg == str(source) or arg.startswith(("-W", "--param=")):
                continue
            flags.append(arg)
        run("transform", [str(args.tool.resolve()), str(source), str(out / "view.c"),
                          str(out / "view.json"),
                          *(["--validated-entry"] if args.validated_entry else []), *flags,
                          *(["--clone-safe-only"] if args.clone_safe_only else []),
                          "-resource-dir=" + str(args.clang_resource_dir.resolve()), "-w"],
            compile_record["directory"])
        command = original.copy()
        command[command.index(str(source))] = str(out / "view.c")
        command[command.index("-o") + 1] = str(out / "view.o")
        command[1:1] = ["-I" + str(out / "include")]
        run("compile", command, compile_record["directory"])
        original_object = (Path(compile_record["directory"]) /
                           original[original.index("-o") + 1]).resolve()
        command = link["arguments"].copy()
        matches = [i for i, item in enumerate(command)
                   if item.endswith(".o") and (Path(link["directory"]) / item).resolve() == original_object]
        if len(matches) != 1:
            raise RuntimeError("object must occur exactly once in the baseline link")
        command[matches[0]] = str(out / "view.o")
        command[command.index("-o") + 1] = str(out / "postgres")
        run("link", command, link["directory"])
        shutil.copytree(args.baseline_prefix, out / "install", copy_function=os.link, symlinks=True)
        (out / "install/bin/postgres").unlink()
        shutil.copy2(out / "postgres", out / "install/bin/postgres")
        report.update(status="built", candidate_sha256=digest(out / "postgres"),
                      source_generated_sha256=digest(out / "view.c"))
    except Exception as error:
        report.update(status="failed", error=str(error))
        raise
    finally:
        (out / "build-report.json").write_text(json.dumps(report, indent=2) + "\n")


if __name__ == "__main__":
    main()
