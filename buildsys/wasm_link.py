#!/usr/bin/env python3
"""Compile and link the embedded kernel with target-only objects and resources."""
from __future__ import annotations

import argparse
import copy
import json
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import re
import shutil
import shlex
import sys

from wasm_kernel import prepare
from wasm_source import source_tree
from wasm_contrib import CONTRIB, selection
from wasm_toolchain import (WasmError, configuration, digest, fingerprint, read_json,
                            run, run_id, write_json, capture)


def target_static_modules(document, selected, major, candidate=False):
    from bundle_embedded_static_modules import validate_manifest
    manifest = copy.deepcopy(document)
    manifest["modules"] = [m for m in manifest["modules"] if m["id"] in selected]
    manifest = validate_manifest(manifest)
    if major != manifest["postgresql_major"] and not candidate:
        raise WasmError("static module major differs from the pinned contract")
    # Every selected module is compiled against these target headers. Keep the
    # registration ABI checks intact and publish the actual compiled major.
    manifest["postgresql_major"] = major
    for module in manifest["modules"]:
        module["postgresql_major"] = major
    return manifest


def source_snapshot(root: Path, extensions=()):
    from wasm_generate import tree_hashes
    result = {name: tree_hashes(root / name) for name in
              ("runtime", "embedded-c", "manifests")}
    result["buildsys"] = {str(p.relative_to(root)): digest(p)
                          for p in sorted((root / "buildsys").iterdir())
                          if p.suffix in (".py", ".mjs", ".mk")}
    # Test C includes are compiled into the bridge but are not TS harness inputs.
    # Retain their identity so a receipt cannot outlive a changed native fixture.
    result["wasm_bridge_sources"] = {str(p.relative_to(root)): digest(p)
        for directory in ("wasm/bridge", "tests/wasm")
        for p in sorted((root / directory).rglob("*"))
        if p.is_file() and (p.suffix in (".c", ".cpp", ".h") or directory == "wasm/bridge" and p.suffix == ".js")}
    if "pgvector" in extensions:
        for name in ("extensions/pgvector", "third_party/pgvector"):
            result[name] = tree_hashes(root / name)
    if any(name in CONTRIB for name in extensions):
        result['extensions/wasm-contrib'] = tree_hashes(root / 'extensions/wasm-contrib')
        for name in extensions:
            if name in CONTRIB: result['postgres/contrib/' + name] = tree_hashes(root / 'postgres/contrib' / name)
    return result


class Builder:
    def __init__(self, config, longjmp, extensions=()):
        self.config, self.root = config, config.root
        self.extensions = selection(','.join(extensions) if extensions else 'none')
        self.longjmp = longjmp
        self.directory, self.inputs, self.tools, self.env = prepare(config, longjmp)
        from check_wasm_sdk_contract import check as check_sdk_contract
        from wasm_storage_protocol import check as check_storage_contract
        sdk_contract = check_sdk_contract(self.root, config.sdk, config.profile["emsdk"]["version"])
        storage_contract = check_storage_contract(self.root)
        self.capture_source = source_tree(config, self.directory, self.inputs)
        self.raw = self.directory / "configure"
        self.source = self.directory / "generated/postgres"
        self.output = self.directory / "link"
        if self.extensions:
            variant = {"extensions": list(self.extensions), "upstream": read_json(
                self.root / "manifests/extensions/pgvector-upstream.json")}
            if any(name in CONTRIB for name in self.extensions):
                variant['contrib'] = self.inputs['postgres_commit']
            self.output = self.directory / "variants" / fingerprint(variant) / "link"
        self.artifacts = self.output / "generated"
        self.current = self.directory / "runs" / ("link-" + run_id())
        self.current.mkdir(parents=True, exist_ok=True)
        self.output.mkdir(parents=True, exist_ok=True)
        self.log = self.current / "build.log"
        self.rows = read_json(self.directory / "compile-db/compile_commands.json")
        self.emcc = str(config.sdk / "upstream/emscripten/emcc")
        self.emxx = str(config.sdk / "upstream/emscripten/em++")
        self.nm = str(Path(self.tools["settings"]["LLVM_ROOT"]) / "llvm-nm")
        self.flags = ["-pthread", f"-sSUPPORT_LONGJMP={longjmp}",
                      *self.inputs["profile"]["modes"][config.mode],
                      f"-ffile-prefix-map={self.root}=postgamma-source",
                      f"-ffile-prefix-map={config.sdk}=emscripten-sdk", "-fdebug-compilation-dir=.",
                      "-fno-strict-aliasing", "-fwrapv",
                      "-D_GNU_SOURCE", "-DEXEC_BACKEND", "-DWAIT_USE_POLL",
                      "-I" + str(self.artifacts / "src/include"),
                      "-I" + str(self.source / "src/include"),
                      "-I" + str(self.root / "runtime/include"),
                      "-I" + str(self.root / "embedded-c/include"),
                      "-I" + str(self.directory / "generated/runtime/include"),
                      "-I" + str(self.output), "-I" + str(self.root)]
        self.objects = []
        self.report = {"kind": "postgamma.wasm-kernel-link", "status": "failed",
                       "inputs": self.inputs, "kernel_linked": False, "sql_validated": False}
        self.report["extensions"] = list(self.extensions)
        self.report["sdk_adapter_contract"] = sdk_contract
        self.report["storage_protocol"] = storage_contract
        receipt = read_json(self.directory / "reports/generate.json")
        generated = read_json(Path(receipt["run_report"]))
        if receipt["status"] != "passed" or generated["status"] != "passed":
            raise WasmError("run make wasm-kernel-generate before linking")
        for name, expected in generated["outputs"].items():
            if digest(self.directory / name) != expected:
                raise WasmError("generated target input changed: " + name)
        self.report["generation_report"] = receipt["run_report"]
        self.candidate_probe = generated.get("candidate_probe", False)
        from wasm_generate import generation_sources, tree_hashes
        for name, expected in generated["inputs"]["sources"].items():
            if tree_hashes(self.root / name) != expected:
                raise WasmError("generation is stale; run make wasm-kernel-generate: " + name)
        if generation_sources(self.root)["generators"] != generated["inputs"]["generators"]:
            raise WasmError("generation tools changed; run make wasm-kernel-generate")
        host = Path(generated["host_tools"]["postgamma-cc"]["path"]).parent
        host_inputs = read_json(host / "inputs.json")
        compiler_sources = {"compiler/" + name: sha for name, sha in tree_hashes(self.root / "compiler").items()}
        if compiler_sources != host_inputs["sources"]:
            raise WasmError("AST tool sources changed; run make wasm-kernel-generate")
        for tool in generated["host_tools"].values():
            if digest(Path(tool["path"])) != tool["sha256"]:
                raise WasmError("AST tool binary changed; run make wasm-kernel-generate")
        self.source_snapshot = self.snapshot()
        self.report["source_snapshot"] = self.source_snapshot
        self.generated_artifacts()
        # Normalize only pg_config's descriptive strings; raw configure inputs
        # remain in the capture and link receipts.
        config_header = (self.artifacts / "src/include/pg_config.h").read_text()
        self.postgres_major = re.search(r'^#define PG_MAJORVERSION "(\d+)"$', config_header, re.M)[1]
        arguments = re.search(r'^#define CONFIGURE_ARGS (.+)$', config_header, re.M)[1]
        self.config_info = self.output / "public-config-info.c"
        self.config_info.write_text('#ifndef FRONTEND\n#include "postgres.h"\n#else\n#include "postgres_fe.h"\n#endif\n'
            '#undef CONFIGURE_ARGS\n#define CONFIGURE_ARGS ' + self.public_build_text(arguments) + '\n#include ' +
            json.dumps(str(self.source / 'src/common/config_info.c')) + '\n')

    def public_build_text(self, value):
        return value.replace(str(self.config.sdk), 'emscripten-sdk').replace(str(self.root), 'postgamma-source')

    def snapshot(self):
        return source_snapshot(self.root, self.extensions)

    def generated_artifacts(self):
        from apply_replacements import apply_bytes
        from install_generated_artifacts import write_artifact
        plan = read_json(self.directory / "plans/postgres-generated.json")
        transformed = {}
        for entry in plan["files"]:
            path = self.raw / entry["path"]
            transformed[str(path.resolve())] = apply_bytes(path.read_bytes(), entry)[0]
        hashes = {}
        for path in sorted(self.raw.rglob("*")):
            if path.suffix not in (".c", ".h", ".inc") or not path.is_file(): continue
            resolved = path.resolve()
            data = transformed.get(str(resolved))
            if data is None:
                template = self.source / resolved.relative_to(self.capture_source) if resolved.is_relative_to(self.capture_source) else path
                data = template.read_bytes()
            target = self.artifacts / path.relative_to(self.raw)
            write_artifact(target, data, 0o644)
            hashes[str(target.relative_to(self.output))] = digest(target)
        # Use PostgreSQL's own installed-path generator for the resource pack's
        # bin/lib/share layout. This changes paths only, not the target ABI.
        override = self.output / "resource-paths.mk"
        override.write_text("override datadir := /postgamma/share\noverride pkglibdir := /postgamma/lib\noverride sysconfdir := /postgamma/etc\n")
        (self.artifacts / "src/port/pg_config_paths.h").unlink(missing_ok=True)
        run(["make", "-C", str(self.artifacts / "src/port"),
             "-f", str(self.raw / "src/port/Makefile"), "-f", str(override), "pg_config_paths.h",
             "top_builddir=" + str(self.raw), "srcdir=" + str(self.source / "src/port"),
             "datadir=/postgamma/share", "pkglibdir=/postgamma/lib", "sysconfdir=/postgamma/etc"],
            self.log, self.env, self.root)
        paths = self.artifacts / "src/port/pg_config_paths.h"
        for name, value in (("PGSHAREDIR", "/postgamma/share"), ("PKGLIBDIR", "/postgamma/lib"), ("SYSCONFDIR", "/postgamma/etc")):
            if f'#define {name} "{value}"' not in paths.read_text():
                raise WasmError("resource layout does not match target paths: " + name)
        hashes[str(paths.relative_to(self.output))] = digest(paths)
        self.report["resource_layout"] = {"datadir": "/postgamma/share", "pkglibdir": "/postgamma/lib",
                                          "sysconfdir": "/postgamma/etc", "paths_sha256": digest(paths)}
        self.report["generated_artifacts"] = {"plan_sha256": digest(self.directory / "plans/postgres-generated.json"),
                                              "replacement_count": plan["summary"]["replacement_count"], "outputs": hashes}

    def target_source(self, source):
        source = Path(source)
        if source.is_relative_to(self.capture_source):
            return self.source / source.relative_to(self.capture_source)
        if source.is_relative_to(self.raw):
            return self.artifacts / source.relative_to(self.raw)
        return source

    def compile(self, source, key, extra=()):
        source = Path(source)
        if source.name == 'config_info.c':
            source = self.config_info
            extra = [self.public_build_text(arg) if arg.startswith('-DVAL_') else arg for arg in extra]
        obj = self.output / "objects" / (key + ".o")
        obj.parent.mkdir(parents=True, exist_ok=True)
        cpp = source.suffix == ".cpp"
        dep = obj.with_suffix(".d")
        command = [self.emxx if cpp else self.emcc, "-std=c++17" if cpp else "-std=gnu11",
                   *self.flags, *extra, "-MMD", "-MF", str(dep), "-c", str(source), "-o", str(obj)]
        stamp = obj.with_suffix(".json")
        identity = {"command": command, "tools": self.tools}
        if stamp.is_file() and obj.is_file():
            old = read_json(stamp)
            if (old.get("identity") == identity and old.get("object") == digest(obj) and
                    all(Path(p).is_file() and digest(Path(p)) == sha for p, sha in old["dependencies"].items())):
                return obj
        run(command, obj.with_suffix(".log"), self.env, self.root)
        with obj.open("rb") as stream:
            if stream.read(8) != b"\0asm\x01\0\0\0":
                raise WasmError("compiler output is not a target object: " + str(obj))
        names = shlex.split(dep.read_text().replace("\\\n", " ").split(":", 1)[1])
        dependencies = {str(Path(p).resolve()): digest(Path(p)) for p in names}
        write_json(stamp, {"identity": identity, "dependencies": dependencies, "object": digest(obj)})
        return obj

    def compile_group(self, jobs):
        with ThreadPoolExecutor(max_workers=self.config.jobs) as executor:
            objects = list(executor.map(lambda job: self.compile(*job), jobs))
        self.objects.extend(objects)
        return objects

    def backend(self):
        jobs = []
        seen = set()
        for row in self.rows:
            source = Path(row["file"])
            outarg = next(a[2:] for a in row["arguments"] if a.startswith("-o"))
            rawobj = (Path(row["directory"]) / outarg).resolve()
            relative = rawobj.relative_to(self.raw)
            name = str(relative)
            if not (name.startswith("src/backend/") or
                    name.startswith(("src/common/", "src/port/")) and name.endswith("_srv.o") or
                    name.startswith("src/timezone/")):
                continue
            if name == "src/backend/main/main.o" or name in seen:
                continue
            seen.add(name)
            source = self.target_source(source)
            # Preserve unit-specific include directories and macro selections from capture.
            extra = [a.replace(str(self.capture_source), str(self.source))
                     if a.startswith("-D") else
                     "-I" + str((Path(row["directory"]) / a[2:]).resolve()).replace(
                         str(self.capture_source), str(self.source))
                     for a in row["arguments"] if a.startswith(("-I", "-D"))]
            extra = [a.replace(str(self.raw), str(self.artifacts)) for a in extra]
            extra += ["-DPOSTGAMMA_BACKEND"]
            jobs.append((source, "backend/" + name[:-2], extra))
        adapter = read_json(self.root / "manifests/postgresql/adapter.json")
        for entry in adapter["generated_support"]["copies"]:
            if entry["target"].endswith(".c"):
                jobs.append((self.source / entry["target"], "runtime/" + Path(entry["target"]).stem,
                             ["-DPOSTGAMMA_BACKEND"]))
        print(f"wasm link: compiling {len(jobs)} backend and shared runtime objects", flush=True)
        self.compile_group(jobs)

    def definitions(self, objects):
        names = set()
        text = capture([self.nm, "-P", "-g", "--defined-only", *(str(p) for p in objects)])
        for line in text.splitlines():
            fields = line.split()
            if len(fields) >= 2 and len(fields[1]) == 1:
                if not re.fullmatch(r"[A-Za-z_][A-Za-z_0-9]*", fields[0]):
                    raise WasmError("unsupported target symbol spelling: " + fields[0])
                names.add(fields[0])
        return names

    def namespace_group(self, name, jobs, keep=(), mapping=None):
        # Clang's asm-name pragma changes linkage names without renaming C tokens,
        # struct members, macros, or the shared ownership identifiers in strings.
        with ThreadPoolExecutor(max_workers=self.config.jobs) as executor:
            original = list(executor.map(lambda job: self.compile(job[0], "unprefixed/" + job[1], job[2]), jobs))
        definitions = self.definitions(original)
        if mapping is None:
            mapping = {n: "postgamma_wasm_" + name + "_" + n for n in definitions
                       if not any(n.startswith(prefix) for prefix in keep)}
        if not set(mapping).issubset(definitions):
            raise WasmError("namespace definition is missing for " + name)
        header = self.output / (name + "-namespace.h")
        # Command-line renames (notably upstream _PG_init) must not expand the
        # pragma's symbol token and accidentally alias two distinct definitions.
        header.write_text("/* Target linkage names; generated from actual definitions. */\n" +
                          "".join(f'#pragma push_macro("{old}")\n#undef {old}\n'
                                  f'#pragma redefine_extname {old} {new}\n#pragma pop_macro("{old}")\n'
                                  for old, new in sorted(mapping.items())))
        with ThreadPoolExecutor(max_workers=self.config.jobs) as executor:
            objects = list(executor.map(lambda job: self.compile(job[0], job[1],
                                       [*job[2], "-include", str(header)]), jobs))
        after = self.definitions(objects)
        expected = (definitions - mapping.keys()) | set(mapping.values())
        if after != expected:
            raise WasmError("target namespace mismatch for " + name + ": " + str(sorted(after ^ expected)))
        archive = self.output / (name + ".a")
        # A fresh archive avoids retaining removed members between invocations.
        temporary = archive.with_suffix(".new.a")
        temporary.unlink(missing_ok=True)
        run([str(self.config.sdk / "upstream/emscripten/emar"), "crs", str(temporary),
             *(str(p) for p in objects)], self.log, self.env, self.root)
        temporary.replace(archive)
        self.objects.append(archive)
        self.report.setdefault("namespaces", {})[name] = {"mapping": mapping, "definitions": sorted(after)}

    def frontend_jobs(self, group, include_libpq=True, tool=False):
        selected = {}
        for row in self.rows:
            source = Path(row["file"])
            outarg = next(a[2:] for a in row["arguments"] if a.startswith("-o"))
            rawobj = (Path(row["directory"]) / outarg).resolve()
            relative = str(rawobj.relative_to(self.raw))
            if not relative.startswith(("src/common/", "src/port/", "src/interfaces/libpq/", "src/fe_utils/")):
                continue
            if relative.endswith(("_srv.o", "_shlib.o")) or relative in selected:
                continue
            if not include_libpq and relative.startswith(("src/interfaces/libpq/", "src/fe_utils/")):
                continue
            source = self.target_source(source)
            extra = [a for a in row["arguments"] if a.startswith("-D")]
            if group == "libpq": extra = [a for a in extra if a != "-DUSE_PRIVATE_ENCODING_FUNCS"]
            extra += ["-I" + str(Path(row["directory"]))]
            extra += ["-I" + str((Path(row["directory"]) / a[2:]).resolve()).replace(
                          str(self.capture_source), str(self.source))
                      for a in row["arguments"] if a.startswith("-I")]
            extra += ["-I" + str(self.source / "src/interfaces/libpq"),
                      "-I" + str(self.source / "src/port"), "-I" + str(self.raw / "src/port")]
            extra = [a.replace(str(self.raw), str(self.artifacts)) for a in extra]
            if tool: extra += ["-DPOSTGAMMA_FRONTEND_TOOL"]
            selected[relative] = (source, group + "/" + relative[:-2].replace("/", "-"), extra)
        return list(selected.values())

    def frontends(self):
        from prepare_private_libpq import prepare as prepare_libpq
        private = self.output / "private-libpq"
        prepare_libpq(self.source, self.root / "manifests/postgresql/embedded.json",
                      private, self.output / "private-libpq-source.json")
        jobs = self.frontend_jobs("libpq")
        hooks = ["-include", str(self.root / "embedded-c/include/postgamma/private/libpq_memory_hooks.h"),
                 "-I" + str(private), "-I" + str(self.source / "src/interfaces/libpq"),
                 "-I" + str(self.source / "src/port"), "-I" + str(self.raw / "src/port"), "-DSO_MAJOR_VERSION=5"]
        jobs = [(private / source.name, key, [*flags, *hooks]) if (private / source.name).is_file()
                else (source, key, flags) for source, key, flags in jobs]
        jobs += [(self.root / "embedded-c/src/libpq_memory_adapter.c", "libpq/adapter", hooks)]
        print("wasm link: compiling private libpq", flush=True)
        self.namespace_group("libpq", jobs, keep=("postgamma_private_libpq_", "postgamma_memory_"))
        jobs = self.frontend_jobs("initdb", tool=True)
        flags = ["-DPOSTGAMMA_FRONTEND_TOOL", "-DUSE_PRIVATE_ENCODING_FUNCS",
                 "-I" + str(self.source / "src/interfaces/libpq"),
                 "-I" + str(self.source / "src/timezone"), "-I" + str(self.source / "src/common")]
        upstream = [*flags, "-DPOSTGAMMA_INITDB_UPSTREAM", "-include",
                    str(self.root / "embedded-c/include/postgamma/private/initdb_runtime.h")]
        # These common files contain per-invocation hooks in the native closure too.
        patched = {"controldata_utils.c", "file_perm.c", "logging.c", "pg_lzcompress.c"}
        jobs = [(source, key, [*extra, *upstream]) if source.name in patched else (source,key,extra)
                for source,key,extra in jobs]
        jobs += [(self.source / "src/bin/initdb/initdb.c", "initdb/main",
                  [*upstream, "-Dmain=postgamma_pg19_initdb_main"]),
                 (self.source / "src/bin/initdb/findtimezone.c", "initdb/findtimezone",
                  [*upstream, "-Dpg_open_tzfile=postgamma_initdb_pg_open_tzfile"]),
                 (self.source / "src/timezone/localtime.c", "initdb/localtime",
                  [*upstream, "-Dpg_open_tzfile=postgamma_initdb_pg_open_tzfile"]),
                 (self.root / "embedded-c/src/postgres_initdb_host.c", "initdb/host", flags),
                 (self.root / "runtime/src/tool_state_runtime.c", "initdb/state", flags)]
        print("wasm link: compiling private initdb", flush=True)
        self.namespace_group("initdb", jobs, keep=("postgamma_initdb_create",))

    def modules(self):
        from bundle_embedded_static_modules import render_facts
        document = read_json(self.root / "manifests/extensions/embedded-static.json")
        document['modules'] += read_json(self.root / 'manifests/extensions/wasm-contrib.json')['modules']
        manifest = target_static_modules(document,
            ("dict_snowball", "plpgsql", "utf8_and_iso8859_1", *self.extensions), int(self.postgres_major), self.candidate_probe)
        facts = render_facts(manifest)
        for module in manifest["modules"]:
            for symbol in module["symbols"]:
                name, link = symbol["logical_name"], symbol["linker_name"]
                if name == "_PG_init": continue
                signature = "const Pg_magic_struct *" if name == "Pg_magic_func" else "const Pg_finfo_record *" if name.startswith("pg_finfo_") else "Datum "
                arguments = "void" if name == "Pg_magic_func" or name.startswith("pg_finfo_") else "FunctionCallInfo"
                if name in ("HnswParallelBuildMain", "IvfflatParallelBuildMain"):
                    signature, arguments = "void ", "dsm_segment *, shm_toc *"
                declaration = f"extern void {link}(void);"
                if facts.count(declaration) != 1:
                    raise WasmError("static module declaration changed: " + link)
                facts = facts.replace(declaration, f"extern {signature}{link}({arguments});")
        (self.output / "postgamma_embedded_static_modules.inc").write_text(
            '#include "postgres.h"\n#include "fmgr.h"\n#include "storage/dsm.h"\n#include "storage/shm_toc.h"\n' + facts)
        self.report["static_modules"] = manifest
        for module in manifest["modules"]:
            name = module["id"]
            if name == "pgvector":
                from wasm_pgvector import build
                build(self, module)
                continue
            if name in CONTRIB:
                from wasm_contrib import build
                build(self, module)
                continue
            if name == "plpgsql":
                sources = [Path(row["file"]) for row in self.rows if "/src/pl/plpgsql/src/" in row["file"]]
                sources = [self.target_source(src) for src in sources]
                extra = ["-I" + str(self.source / "src/pl/plpgsql/src"), "-I" + str(self.artifacts / "src/pl/plpgsql/src")]
            elif name == "dict_snowball":
                directory = self.source / "src/backend/snowball"
                sources = [directory / "dict_snowball.c", *sorted((directory / "libstemmer").glob("*.c"))]
                extra = ["-I" + str(self.source / "src/include/snowball"),
                         "-I" + str(self.source / "src/include/snowball/libstemmer")]
            elif name == "utf8_and_iso8859_1":
                sources = [self.source / "src/backend/utils/mb/conversion_procs" / name / (name + ".c")]
                extra = []
            else:
                raise WasmError("unsupported Wasm static module: " + name)
            jobs = [(src, "modules/" + name + "/" + src.stem, ["-DPOSTGAMMA_BACKEND", *extra]) for src in sources]
            mapping = {entry["logical_name"]: entry["linker_name"] for entry in module["symbols"]}
            self.namespace_group(name, jobs, mapping=mapping)

    def product(self):
        policy = read_json(self.root / "manifests/profiles/wasm-resource-budget.json")
        io_stack = next(role["stack_bytes"] for role in policy["thread_roles"] if role["role"] == "io_owner")
        pool = sum(role["count"] for role in policy["thread_roles"])
        excluded = {"bootstrap_probe.c", "libpq_memory_adapter.c", "postgres_initdb_host.c",
                    "postgres_logical_tool_host.c", "postgamma_logical_management.c"}
        jobs = []
        for source in sorted((self.root / "embedded-c/src").glob("*.c")):
            if source.name in excluded: continue
            extra = []
            if source.name == "postgres_bootstrap_bridge.c":
                extra += ["-Dpgm_bootstrap_run=postgamma_private_bootstrap_probe"]
            if source.name == "postgamma.c":
                extra += ['-DPOSTGAMMA_PRODUCT_VERSION="' + (self.root / 'VERSION').read_text().strip() + '"', '-DPOSTGAMMA_POSTGRESQL_MAJOR="' + self.postgres_major + '"']
            jobs.append((source, "product/" + source.stem, extra))
        jobs.append((self.root / "runtime/src/postgres_embedded_kernel.c", "product/kernel", ["-DPOSTGAMMA_BACKEND"]))
        for source in sorted((self.root / "runtime/platform/emscripten").glob("*")):
            if source.suffix not in (".c", ".cpp"): continue
            extra = ["-isystem", str(self.config.sdk / "upstream/emscripten/system/lib/wasmfs")] if source.suffix == ".cpp" else []
            if source.name == "storage.cpp":
                extra += [f"-DPOSTGAMMA_WASM_IO_STACK_BYTES={io_stack}",
                          f"-DPOSTGAMMA_WASM_IO_CHANNEL_COUNT={pool}"]
            jobs.append((source, "platform/" + source.stem, extra))
        self.compile_group(jobs)

    def resources(self):
        resources = self.output / "resources"
        if resources.exists():
            shutil.rmtree(resources)
        share = resources / "share"
        for name in ("bin", "lib", "share"):
            (resources / name).mkdir(parents=True, exist_ok=True)
        # The C entry point supplies this location anchor to PG path discovery.
        # It contains no native executable and is never executed.
        (resources / "bin/postgres").write_text("postgamma.wasm resource-location anchor\n")
        write_json(resources / "lib/static-modules.json", self.report["static_modules"])
        # BKI and constraints are generated data, not compilation headers.
        copies = {"postgres.bki": self.raw / "src/include/catalog/postgres.bki"}
        copies["system_constraints.sql"] = self.raw / "src/include/catalog/system_constraints.sql"
        for name in ("information_schema.sql", "system_functions.sql", "system_views.sql", "sql_features.txt"):
            copies[name] = self.source / "src/backend/catalog" / name
        for name in ("pg_hba.conf.sample", "pg_ident.conf.sample", "pg_hosts.conf.sample"):
            copies[name] = self.source / "src/backend/libpq" / name
        copies["postgresql.conf.sample"] = self.source / "src/backend/utils/misc/postgresql.conf.sample"
        snowball = self.source / "src/backend/snowball"
        run(["perl", str(snowball / "snowball_create.pl"), "--input", str(snowball), "--outdir", str(share)],
            self.log, self.env, self.root)
        for name, source in copies.items():
            shutil.copy2(source, share / name)
        for source in sorted((snowball / "stopwords").glob("*.stop")):
            target = share / "tsearch_data" / source.name
            target.parent.mkdir(exist_ok=True)
            shutil.copy2(source, target)
        for source in sorted((self.source / "src/timezone/tznames").iterdir()):
            if source.name == "Makefile" or not source.is_file(): continue
            target = share / "timezonesets" / source.name
            target.parent.mkdir(exist_ok=True)
            shutil.copy2(source, target)
        for module in self.report["static_modules"]["modules"]:
            for resource in module["resources"]:
                source = self.root / resource["source"]
                target = resources / resource["logical_path"]
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(source, target)
        if "pgvector" in self.extensions:
            target = share / "licenses/pgvector-LICENSE"
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(self.root / "third_party/pgvector/LICENSE", target)
        receipt = read_json(self.directory / "reports/capture.json")
        captured = read_json(Path(receipt["run_report"]))
        zic = Path(captured["configure"]["host_zic"])
        run([str(zic), "-d", str(share / "timezone"), str(self.source / "src/timezone/data/tzdata.zi")],
            self.log, self.env, self.root)
        self.report["resources"] = {"postgres_commit": self.inputs["postgres_commit"],
                                   "host_zic": {"path": str(zic), "sha256": digest(zic)},
                                   "files": {str(p.relative_to(resources)): digest(p) for p in sorted(resources.rglob("*")) if p.is_file()}}
        return resources

    def link(self, bridge_source=None, bridge_flags=()):
        resources = self.resources()
        if bridge_source is not None:
            self.report["bridge"] = {"path": str(bridge_source), "sha256": digest(bridge_source), "flags": list(bridge_flags)}
            bridge = self.compile(bridge_source, "bridge/kernel-smoke", extra=bridge_flags)
            self.objects.append(bridge)
        from wasm_dependency_inventory import classify_symbol
        defined = self.definitions(self.objects)
        references = {}
        for line in capture([self.nm, "-A", "-u", "-P", *(str(p) for p in self.objects)]).splitlines():
            match = re.fullmatch(r"(.+): (\S+) U(?:\s.*)?", line)
            if match and match[2] not in defined:
                references.setdefault(match[2], []).append(match[1])
        self.report["target_library_references"] = {
            name: {"native_inventory_category": classify_symbol(name)["category"],
                   "objects": paths} for name, paths in sorted(references.items())}
        policy = read_json(self.root / "manifests/profiles/wasm-resource-budget.json")
        site = self.output / "site"
        site.mkdir(parents=True, exist_ok=True)
        output = site / "postgamma.mjs"
        # SDK worker pools remain within the reviewed initial pthread budget.
        pool = sum(role["count"] for role in policy["thread_roles"])
        roots = ("pgm_instance_open", "pgm_instance_close", "pgm_connection_open", "pgm_connection_close",
                 "pgm_execute", "pgm_result_value", "pgm_result_free", "pgm_error_message", "pgm_error_free")
        self.report["mailbox_notification"] = "postMessage"
        self.report["storage_notification"] = "direct BroadcastChannel"
        command = [self.emxx, "-pthread", "-sSUPPORT_LONGJMP=" + self.longjmp,
                   "--js-library", str(self.root / "runtime/platform/emscripten/mailbox.js"),
                   *(["--extern-pre-js", str(self.root / "wasm/bridge/worker-messages.js")]
                     if bridge_source == self.root / "wasm/bridge/sql.c" else []),
                   "-sEXPORTED_FUNCTIONS=" + json.dumps(["_" + name for name in (*roots, "emscripten_proxy_finish", "emscripten_proxy_execute_queue", "postgamma_storage_finish")]),
                   *("-Wl,--wrap=" + name for name in ("__wasi_fd_sync", "__syscall_fdatasync", "__syscall_openat", "__syscall_mkdirat")),
                   "-Wl,--fatal-warnings", "-Wl,-Map=" + str(self.current / "link.map"),
                   *("-Wl,--export=" + name for name in ("__tls_size", "__tls_align", "__data_end", "__heap_base")),
                   *("-Wl,--undefined=" + name for name in roots),
                   *self.inputs["profile"]["modes"][self.config.mode],
                   *(["-Wl,--wrap=postgamma_supervisor_ticket_wait"] if "-DPOSTGAMMA_WASM_TESTING" in bridge_flags else []),
                   "--no-entry", "-sMODULARIZE=1", "-sEXPORT_ES6=1", "-sENVIRONMENT=worker",
                   '-sINCOMING_MODULE_JS_API=["print","printErr","onAbort","locateFile","getPreloadedPackage","wasmBinary","noExitRuntime","instantiateWasm","wasmMemory"]',
                   '-sEXPORTED_RUNTIME_METHODS=["HEAPU8","PThread","wasmMemory"]', f"-sPTHREAD_POOL_SIZE={pool}",
                   "-sPTHREAD_POOL_SIZE_STRICT=2", "-sALLOW_BLOCKING_ON_MAIN_THREAD=0",
                   f"-sINITIAL_MEMORY={policy['initial_memory_bytes']}", f"-sMAXIMUM_MEMORY={policy['maximum_memory_bytes']}",
                   "-sALLOW_MEMORY_GROWTH=1", "-sABORTING_MALLOC=0", f"-sSTACK_SIZE={policy['control_stack_bytes']}",
                   "-sDEFAULT_PTHREAD_STACK_SIZE=4194304", "-sSTACK_OVERFLOW_CHECK=2", "-sWASMFS=1", "-sASSERTIONS=2",
                   "--preload-file", str(resources) + "@/postgamma",
                   *(str(p) for p in self.objects), "-o", output.name]
        response = self.current / "link.rsp"
        response.write_text(shlex.join(command[1:]) + "\n")
        run([command[0], "@" + str(response)], self.log, self.env, site)
        audit = self.current / "imports.json"
        run([sys.executable, str(self.root / "buildsys/wasm_link_audit.py"),
             "--sdk", str(self.config.sdk), "--wasm", str(site / "postgamma.wasm"),
             "--output", str(audit)], self.log, self.env, self.root)
        self.report["audit"] = read_json(audit)
        measured = self.report["audit"]["layout"]
        if measured["__data_end"] > policy["static_reserve_bytes"] or measured["__tls_size"] > policy["tls_reserve_per_worker_bytes"]:
            raise WasmError("linked state exceeds the provisional static/TLS budget")
        if self.snapshot() != self.source_snapshot:
            raise WasmError("kernel link inputs changed during the build; rerun with stable inputs")
        if bridge_source is not None and digest(bridge_source) != self.report["bridge"]["sha256"]:
            raise WasmError("bridge changed during the build")
        self.report["kernel_linked"] = True
        self.report["status"] = "passed"
        self.report["link_command"] = command
        self.report["outputs"] = {p.name: digest(p) for p in sorted(site.glob("postgamma.*")) if p.is_file()}

    def finish(self):
        self.report["objects"] = {str(p.relative_to(self.directory)): digest(p) for p in self.objects}
        write_json(self.current / "report.json", self.report)
        write_json(self.directory / "reports/link.json", {"status": self.report["status"],
                   "run_report": str(self.current / "report.json")})
        print(f"wasm link: {self.report['status']}; {self.current / 'report.json'}", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", required=True, type=Path)
    for name in ("build-dir", "deps-dir", "sdk-dir", "profile"):
        parser.add_argument("--" + name, type=Path)
    parser.add_argument("--target", default="wasm32-emscripten-pthreads")
    parser.add_argument("--mode", default="debug", choices=("debug", "release"))
    parser.add_argument("--longjmp", default="emscripten")
    parser.add_argument("--node", default="")
    parser.add_argument("--browser-executable", default="")
    parser.add_argument("--jobs", type=int, default=2)
    args = parser.parse_args()
    builder = None
    try:
        builder = Builder(configuration(args), args.longjmp)
        builder.backend()
        builder.frontends()
        builder.modules()
        builder.product()
        builder.link()
    except Exception as error:
        if builder: builder.report["error"] = str(error)
        parser.exit(2, "wasm link: " + str(error).splitlines()[0] + "\n")
    finally:
        if builder: builder.finish()


if __name__ == "__main__":
    main()
