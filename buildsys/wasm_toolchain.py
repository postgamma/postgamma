#!/usr/bin/env python3
"""Resolve and install the isolated tools used by WebAssembly probes."""

from __future__ import annotations

import ast
from dataclasses import dataclass
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import signal
import shutil
import subprocess
import sys
import uuid


class WasmError(RuntimeError):
    """A required tool, input, or probe is unavailable or inconsistent."""


def read_json(path: Path) -> dict:
    return json.loads(path.read_text(encoding="utf-8"))


def write_json(path: Path, value: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    data = json.dumps(value, indent=2, sort_keys=True) + "\n"
    if not path.exists() or path.read_text(encoding="utf-8") != data:
        temporary = path.with_name(path.name + ".tmp")
        temporary.write_text(data, encoding="utf-8")
        temporary.replace(path)


def digest(path: Path) -> str:
    result = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            result.update(chunk)
    return result.hexdigest()


def fingerprint(value: dict) -> str:
    return hashlib.sha256(json.dumps(value, sort_keys=True).encode()).hexdigest()[:16]


def run_id() -> str:
    return datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ-") + uuid.uuid4().hex[:8]


def capture(command: list[str], env: dict | None = None, cwd: Path | None = None) -> str:
    result = subprocess.run(command, env=env, cwd=cwd, text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=30)
    if result.returncode:
        raise WasmError(f"command failed: {command!r}\n{result.stdout}{result.stderr}")
    return result.stdout.strip()


def run(command: list[str], log: Path, env: dict | None = None,
        cwd: Path | None = None, timeout: int = 1200) -> None:
    log.parent.mkdir(parents=True, exist_ok=True)
    with log.open("a", encoding="utf-8") as output:
        output.write(json.dumps({"command": command, "cwd": str(cwd)}) + "\n")
        output.flush()
        process = subprocess.Popen(command, env=env, cwd=cwd, stdout=output,
                                   stderr=subprocess.STDOUT, start_new_session=True)
        try:
            process.wait(timeout=timeout)
        except BaseException:
            # Only terminate descendants of this invocation, including a hung browser.
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            process.wait()
            raise
    if process.returncode:
        tail = "\n".join(log.read_text(encoding="utf-8", errors="replace").splitlines()[-40:])
        raise WasmError(f"command failed ({process.returncode}); log: {log}\n{tail}")


def validate_directories(root: Path, build: Path, deps: Path, sdk: Path) -> None:
    if root == build or build in root.parents:
        raise WasmError("BUILD_DIR must not contain the source checkout")
    for cache in (deps, sdk, root / "wasm/node_modules"):
        cache = cache.resolve()
        if cache == build or build in cache.parents:
            raise WasmError(f"tool/cache directory must be outside BUILD_DIR: {cache}")
    if deps == root or deps in root.parents:
        raise WasmError("WASM_DEPS_DIR must not contain the source checkout")
    if (build / "wasm").is_symlink():
        raise WasmError("Wasm output directory must not be a symlink")


@dataclass
class Configuration:
    root: Path
    build: Path
    deps: Path
    sdk: Path
    profile_path: Path
    profile: dict
    mode: str
    external_sdk: bool
    node_override: str = ""
    browser_override: str = ""
    jobs: int = 2

    @property
    def target(self) -> str:
        return self.profile["target"]

    @property
    def harness(self) -> Path:
        return self.root / "tests/wasm/browser/run.mjs"

    @property
    def package(self) -> Path:
        return self.root / "wasm"


def configuration(args) -> Configuration:
    root = args.root.resolve()
    build = (args.build_dir or root / "build").resolve()
    deps = (args.deps_dir or root / ".deps/wasm").resolve()
    profile_path = (args.profile or root / "manifests/profiles/wasm-emscripten.json").resolve()
    profile = read_json(profile_path)
    if args.jobs < 1:
        raise WasmError("JOBS must be positive")
    if (profile.get("schema_version") != 1 or
            profile.get("kind") != "postgamma.wasm-probe-profile"):
        raise WasmError("unsupported Wasm probe profile")
    if profile["target"] != args.target or args.target != "wasm32-emscripten-pthreads":
        raise WasmError("only the wasm32-emscripten-pthreads probe target is implemented")
    if args.mode not in profile["modes"]:
        raise WasmError(f"mode is not configured: {args.mode}")
    sdk = (args.sdk_dir or deps / "emsdk" / profile["emsdk"]["version"]).resolve()
    validate_directories(root, build, deps, sdk)
    return Configuration(root, build, deps, sdk, profile_path, profile, args.mode,
                         args.sdk_dir is not None, args.node, args.browser_executable, args.jobs)


def sdk_tools(config: Configuration) -> tuple[dict, dict]:
    emscripten = config.sdk / "upstream/emscripten"
    emcc = emscripten / "emcc.py"
    sdk_config = config.sdk / ".emscripten"
    if not emcc.is_file() or not sdk_config.is_file():
        raise WasmError(f"Emscripten is not installed/activated at {config.sdk}; run make wasm-setup")
    env = dict(os.environ)
    # Do not inherit another SDK's cache, compiler flags, or configuration.
    for key in list(env):
        if key.startswith(("EMSDK_", "EM_", "EMCC_")) or key == "_EM_TEST_RUNNER":
            env.pop(key)
    env.update(EMSDK=str(config.sdk), EM_CONFIG=str(sdk_config),
               EMCC_CORES=str(config.jobs), EMSDK_NUM_CORES=str(config.jobs),
               PLAYWRIGHT_BROWSERS_PATH=str(config.deps / "browsers"),
               npm_config_cache=str(config.deps / "npm-cache"),
               npm_config_update_notifier="false")
    node_value = capture([sys.executable, str(emscripten / "em-config.py"), "NODE_JS"], env)
    node = ast.literal_eval(node_value)
    if isinstance(node, list):
        node = node[0]
    node_path = Path(config.node_override or node).resolve()
    if not node_path.is_file():
        raise WasmError(f"Node.js is missing: {node_path}; run make wasm-setup")
    env["PATH"] = os.pathsep.join((str(node_path.parent), str(emscripten), env.get("PATH", "")))
    node_version = capture([str(node_path), "--version"], env)
    if int(node_version.lstrip("v").split(".")[0]) < 20:
        raise WasmError("Wasm browser tools require Node.js 20 or newer")
    npm = shutil.which("npm", path=env["PATH"])
    if not npm:
        raise WasmError("npm is missing from the selected Node.js toolchain")
    emcc_version = capture([sys.executable, str(emcc), "--version"], env).splitlines()[0]
    expected = config.profile["emsdk"]["version"]
    version_match = re.search(r"\) (\d+\.\d+\.\d+)(?: |$)", emcc_version)
    if not version_match or version_match.group(1) != expected:
        raise WasmError(f"profile requires Emscripten {expected}; found {emcc_version}")
    settings = {name: capture([sys.executable, str(emscripten / "em-config.py"), name], env)
                for name in ("LLVM_ROOT", "BINARYEN_ROOT", "CACHE", "PORTS")}
    for name in ("CACHE", "PORTS"):
        cache = Path(settings[name]).resolve()
        if cache == config.build or config.build in cache.parents:
            raise WasmError(f"SDK {name} must be outside BUILD_DIR: {cache}")
    clang = Path(settings["LLVM_ROOT"]) / "clang"
    wasm_ld = Path(settings["LLVM_ROOT"]) / "wasm-ld"
    wasm_opt = Path(settings["BINARYEN_ROOT"]) / "bin/wasm-opt"
    sdk_revision = None
    if (config.sdk / ".git").exists():
        sdk_revision = capture(["git", "-C", str(config.sdk), "rev-parse", "HEAD"])
    if not config.external_sdk and sdk_revision != config.profile["emsdk"]["commit"]:
        raise WasmError(f"managed emsdk revision mismatch at {config.sdk}")
    identity = {
        "sdk": str(config.sdk), "external_sdk": config.external_sdk,
        "emsdk_revision": sdk_revision,
        "emcc": str(emcc), "emcc_version": emcc_version,
        "emcc_sha256": digest(emcc), "sdk_config_sha256": digest(sdk_config),
        "clang_version": capture([str(clang), "--version"], env).splitlines()[0],
        "clang_sha256": digest(clang),
        "wasm_ld_version": capture([str(wasm_ld), "--version"], env),
        "wasm_ld_sha256": digest(wasm_ld),
        "wasm_opt_version": capture([str(wasm_opt), "--version"], env),
        "wasm_opt_sha256": digest(wasm_opt), "settings": settings,
        "node": str(node_path), "node_version": node_version,
        "npm": npm, "npm_version": capture([npm, "--version"], env),
        "python": sys.version.split()[0],
    }
    return identity, env


def package_inputs(config: Configuration) -> dict:
    return {name: digest(config.package / name) for name in ("package.json", "package-lock.json")}


def browser_description(config: Configuration, tools: dict, env: dict) -> dict:
    command = [tools["node"], str(config.harness), "--root", str(config.root), "--describe"]
    if config.browser_override:
        command += ["--executable", str(Path(config.browser_override).resolve())]
    return json.loads(capture(command, env))


def browser_environment(config: Configuration, env: dict) -> dict:
    result = dict(env)
    local = config.deps / "browser-libs/root"
    directories = sorted(local.glob("usr/lib/*-linux-gnu")) + sorted(local.glob("lib/*-linux-gnu"))
    if directories:
        paths = [str(path) for path in directories]
        if result.get("LD_LIBRARY_PATH"):
            paths.append(result["LD_LIBRARY_PATH"])
        result["LD_LIBRARY_PATH"] = os.pathsep.join(paths)
    return result


def missing_browser_libraries(executable: Path, env: dict) -> list[str]:
    if platform.system() != "Linux":
        return []
    result = capture(["ldd", str(executable)], env)
    return sorted(re.findall(r"^\s*(\S+) => not found$", result, re.MULTILINE))


def apt_download_plan(output: str) -> list[str]:
    packages = []
    for line in output.splitlines():
        if not line.startswith("Inst "):
            continue
        match = re.match(r"Inst (\S+)(?: \[[^]]+\])? \((\S+) ", line)
        if not match:
            raise WasmError(f"unrecognized apt simulation output: {line}")
        name, version = match.groups()
        if name.split(":")[0] in {"libc6", "libc-bin"}:
            raise WasmError("browser libraries require a host libc upgrade; install system dependencies first")
        packages.append(f"{name}={version}")
    return packages


def setup_browser_libraries(config: Configuration, executable: Path, env: dict, log: Path) -> bool:
    missing = missing_browser_libraries(executable, browser_environment(config, env))
    if not missing:
        return False
    release = {}
    for line in Path("/etc/os-release").read_text().splitlines():
        if "=" in line:
            key, value = line.split("=", 1)
            release[key] = value.strip('"')
    host = f"{release.get('ID')}-{release.get('VERSION_ID')}-{platform.machine()}"
    packages = config.profile.get("browser_host_packages", {}).get(host)
    if not packages:
        raise WasmError(f"missing browser libraries: {', '.join(missing)}; "
                        "install Playwright's Chromium system dependencies for this host")
    print("wasm setup: caching Chromium host libraries without system installation", flush=True)
    local = config.deps / "browser-libs"
    downloads = local / "downloads"
    downloads.mkdir(parents=True, exist_ok=True)
    plan = capture(["apt-get", "--simulate", "--no-install-recommends", "--no-upgrade", "install", *packages],
                   dict(env, LC_ALL="C"))
    (local / "apt-plan.txt").write_text(plan + "\n", encoding="utf-8")
    requested = apt_download_plan(plan)
    if not requested:
        raise WasmError("browser libraries are missing despite installed packages; repair the host libraries")
    run(["apt-get", "download", *requested], log, env, downloads)
    archives = sorted(downloads.glob("*.deb"))
    for archive in archives:
        run(["dpkg-deb", "--extract", str(archive), str(local / "root")], log, env)
    remaining = missing_browser_libraries(executable, browser_environment(config, env))
    if remaining:
        raise WasmError(f"browser libraries still missing: {', '.join(remaining)}")
    write_json(local / "receipt.json", {"host": host, "packages": requested,
                                       "archives": {p.name: digest(p) for p in archives}})
    return True


def package_ready(config: Configuration, tools: dict, env: dict) -> bool:
    receipt = config.deps / "receipts/npm.json"
    installed_lock = config.package / "node_modules/.package-lock.json"
    if not receipt.is_file() or not installed_lock.is_file():
        return False
    state = read_json(receipt)
    if (state.get("inputs") != package_inputs(config) or
            state.get("installed_lock_sha256") != digest(installed_lock)):
        return False
    try:
        description = browser_description(config, tools, env)
        return description["playwright"] == read_json(config.package / "package.json")["devDependencies"]["playwright"]
    except (WasmError, OSError, ValueError):
        return False


def setup(config: Configuration) -> None:
    directory = config.deps / "receipts" / ("setup-" + run_id())
    directory.mkdir(parents=True)
    log = directory / "commands.log"
    report = {"kind": "postgamma.wasm-setup", "status": "failed", "actions": []}
    try:
        sdk = config.profile["emsdk"]
        if not config.external_sdk:
            if not config.sdk.exists():
                config.sdk.parent.mkdir(parents=True, exist_ok=True)
                print(f"wasm setup: downloading emsdk {sdk['version']}", flush=True)
                run(["git", "clone", "--depth", "1", "--branch", sdk["version"],
                     sdk["repository"], str(config.sdk)], log)
                report["actions"].append("download-sdk-manager")
            actual = capture(["git", "-C", str(config.sdk), "rev-parse", "HEAD"])
            if actual != sdk["commit"]:
                raise WasmError(f"managed emsdk revision mismatch at {config.sdk}")
            if capture(["git", "-C", str(config.sdk), "status", "--porcelain", "--untracked-files=no"]):
                raise WasmError(f"managed emsdk source has local changes: {config.sdk}")
            try:
                sdk_tools(config)
            except (WasmError, OSError):
                print(f"wasm setup: installing emsdk {sdk['version']}", flush=True)
                run([sys.executable, str(config.sdk / "emsdk.py"), "install", sdk["version"]], log)
                run([sys.executable, str(config.sdk / "emsdk.py"), "activate", sdk["version"]], log)
                report["actions"].append("install-sdk")
        tools, env = sdk_tools(config)
        report["tools"] = tools
        if not package_ready(config, tools, env):
            print("wasm setup: installing locked npm dependencies", flush=True)
            run([tools["npm"], "ci", "--ignore-scripts", "--no-audit", "--no-fund"],
                log, env, config.package)
            write_json(config.deps / "receipts/npm.json", {
                "inputs": package_inputs(config),
                "installed_lock_sha256": digest(config.package / "node_modules/.package-lock.json"),
            })
            report["actions"].append("install-npm")
        description = browser_description(config, tools, env)
        if not Path(description["executable"]).is_file():
            if config.browser_override:
                raise WasmError(f"supplied browser is missing: {config.browser_override}")
            print("wasm setup: installing pinned Chromium", flush=True)
            run([tools["node"], str(config.package / "node_modules/playwright/cli.js"),
                 "install", "chromium", "--no-shell"], log, env)
            report["actions"].append("install-browser")
        if setup_browser_libraries(config, Path(description["executable"]), env, log):
            report["actions"].append("cache-browser-libraries")
        report["browser"] = description
        report["status"] = "installed"
        write_json(config.deps / "receipts/tools.json", report)
        print("wasm setup: " + (", ".join(report["actions"]) or "reused SDK, npm dependencies, and browser"))
    except Exception as error:
        report["error"] = str(error)
        raise
    finally:
        write_json(directory / "report.json", report)
        print(f"wasm setup report: {directory / 'report.json'}", flush=True)
