#!/usr/bin/env python3
"""Link isolated runtime experiments while verifying the unchanged baseline."""
import argparse
from concurrent.futures import ThreadPoolExecutor
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
    parser.add_argument("--spec", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--jobs", type=int, default=1)
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error("--jobs must be positive")
    spec = json.loads(args.spec.read_text())
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    link = json.loads(Path(spec["link_record"]).read_text())
    prefix = Path(spec["baseline_prefix"])
    report = {"status": "running", "scope": "diagnostic runtime experiment",
              "spec": spec, "commands": [], "baseline_sha256": digest(prefix / "bin/postgres")}
    if spec.get("view_tool"):
        report["view_tool_sha256"] = digest(spec["view_tool"])

    def run(name, command, cwd):
        report["commands"].append({"name": name, "arguments": command, "directory": str(cwd)})
        with (out / (name + ".log")).open("w") as log:
            result = subprocess.run(command, cwd=cwd, stdout=log,
                                    stderr=subprocess.STDOUT, timeout=600)
        if result.returncode:
            raise RuntimeError(f"{name} failed ({result.returncode}); see {out / (name + '.log')}")

    try:
        command = link["arguments"].copy()
        command[command.index("-o") + 1] = str(out / "baseline-postgres")
        run("baseline-link", command, link["directory"])
        if digest(out / "baseline-postgres") != report["baseline_sha256"]:
            raise RuntimeError("captured link no longer reproduces the baseline")
        for target, source in spec.get("headers", {}).items():
            dest = out / "include" / target
            dest.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, dest)
        command = link["arguments"].copy()
        report["units"] = []
        compilations = []
        linked_objects = {}
        for index, item in enumerate(command):
            if item.endswith(".o"):
                path = (Path(link["directory"]) / item).resolve()
                linked_objects.setdefault(path, []).append(index)
        for number, unit in enumerate(spec["units"]):
            record = json.loads(Path(unit["compile_record"]).read_text())
            source = out / (str(number) + "-" + Path(unit["source"]).name)
            shutil.copy2(unit["source"], source)
            compiler = record["arguments"].copy()
            old_object = (Path(record["directory"]) / compiler[compiler.index("-o") + 1]).resolve()
            compiler[compiler.index(record["file"])] = str(source)
            new_object = out / (str(number) + ".o")
            compiler[compiler.index("-o") + 1] = str(new_object)
            compiler[1:1] = ["-I" + str(out / "include"),
                            "-iquote", str(Path(record["file"]).parent),
                            *spec.get("flags", [])]
            transform = None
            # State accesses can be hidden entirely inside included PG macros.
            if spec.get("view_tool"):
                flags = []
                skip = False
                for argument in compiler[1:]:
                    if skip:
                        skip = False
                        continue
                    if argument == "-o":
                        skip = True
                    elif argument not in ("-c", str(source)) and not argument.startswith(("-W", "--param=")):
                        flags.append(argument)
                candidate = out / (str(number) + "-view.c")
                facts = out / (str(number) + "-view.json")
                transform = ("transform-" + str(number),
                             [spec["view_tool"], str(source), str(candidate), str(facts),
                              "--borrowed-scope", *spec.get("view_flags", []), *flags,
                              "-resource-dir=" + spec["clang_resource_dir"], "-w"],
                             record["directory"])
                compiler[compiler.index(str(source))] = str(candidate)
            compilations.append((transform, ("compile-" + str(number), compiler, record["directory"])))
            matches = linked_objects.pop(old_object, [])
            if len(matches) != 1:
                raise RuntimeError("replacement must match exactly one linked object")
            command[matches[0]] = str(new_object)
            report["units"].append({"source": str(source), "sha256": digest(source)})
        def compile_unit(transform, compilation):
            if transform is not None:
                run(*transform)
            run(*compilation)

        with ThreadPoolExecutor(max_workers=args.jobs) as pool:
            futures = [pool.submit(compile_unit, *arguments) for arguments in compilations]
            for future in futures:
                future.result()
        report["transformed_sources"] = {
            path.name: digest(path) for path in sorted(out.glob("*-view.c"))}
        command[command.index("-o") + 1] = str(out / "postgres")
        run("link", command, link["directory"])
        shutil.copytree(prefix, out / "install", copy_function=os.link, symlinks=True)
        (out / "install/bin/postgres").unlink()
        shutil.copy2(out / "postgres", out / "install/bin/postgres")
        report.update(status="built", candidate_sha256=digest(out / "postgres"))
    except Exception as error:
        report.update(status="failed", error=str(error))
        raise
    finally:
        (out / "build-report.json").write_text(json.dumps(report, indent=2) + "\n")


if __name__ == "__main__":
    main()
