#!/usr/bin/env python3
"""Cache the pinned browser matrix and its headless host libraries."""

from pathlib import Path
import os
import platform
import shlex
import subprocess
import sys
import tempfile

from wasm_toolchain import (Configuration, WasmError, apt_download_plan, browser_environment,
                            capture, digest, package_ready, read_json, run, run_id, sdk_tools, write_json)


def check_browsers(config: Configuration) -> Path:
    """Check installed browser dependencies and launch every engine; never install."""
    directory = config.build / "wasm/browser-doctor" / run_id()
    directory.mkdir(parents=True)
    report = {"kind": "postgamma.wasm-browser-doctor", "status": "failed",
              "scope": "Installed browser libraries and startup; no SQL kernel acceptance"}
    try:
        tools, env = sdk_tools(config)
        if not package_ready(config, tools, env):
            raise WasmError("locked npm dependencies are missing or changed; run make wasm-setup")
        info = description(config, tools, env)
        if config.browser_override:
            for engine in info["engines"]:
                if engine["name"] == "chromium":
                    engine["executable"] = str(Path(config.browser_override).resolve())
        report["browsers"] = info
        absent = [engine["name"] for engine in info["engines"]
                  if not Path(engine["executable"]).is_file()]
        if absent:
            raise WasmError("missing browsers: " + ", ".join(absent) + "; run make wasm-browser-setup")
        if platform.system() == "Linux":
            missing = missing_libraries(config, info["engines"], env)
            report["missing_libraries"] = missing
            if missing:
                raise WasmError("browser host libraries unavailable; run make wasm-browser-setup: "
                                + "; ".join(missing))
        output = directory / "startup.json"
        report["startup_report"] = str(output)
        command = [tools["node"], str(config.root / "tests/wasm/browser/doctor.mjs"),
                   "--root", str(config.root), "--output", str(output)]
        if config.browser_override:
            command += ["--executable", str(Path(config.browser_override).resolve())]
        launcher = config.deps / "browser-libs/launch-webkit.sh"
        if launcher.is_file():
            command += ["--webkit-executable", str(launcher)]
        with tempfile.TemporaryDirectory(prefix="pgm-browser-doctor-") as temporary:
            run(command, directory / "startup.log",
                dict(browser_environment(config, env), TMPDIR=temporary), config.root, timeout=90)
        startup = read_json(output)
        report["startup"] = startup
        expected = {"chromium", "firefox", "webkit"}
        rows = startup.get("engines", [])
        if (startup.get("status") != "passed" or len(rows) != 3 or
                {row["name"] for row in rows} != expected or
                any(row.get("status") != "passed" for row in rows)):
            raise WasmError("browser startup matrix incomplete or failed; see " + str(output))
        report["status"] = "passed"
        print("wasm browsers: " + ", ".join(f"{r['name']} {r['version']}" for r in rows), flush=True)
    except Exception as error:
        report["error"] = str(error)
        raise
    finally:
        write_json(directory / "report.json", report)
        print(f"wasm browser doctor report: {directory / 'report.json'}", flush=True)
    return directory / "report.json"


def description(config: Configuration, tools: dict, env: dict) -> dict:
    import json
    return json.loads(capture([tools["node"], str(config.root / "tests/wasm/browser/deployment.mjs"),
                               "--root", str(config.root), "--describe"], env))


def headless_files(engine: dict) -> tuple[list[Path], list[Path]]:
    executable = Path(engine["executable"])
    if engine["name"] == "firefox":
        return [executable, executable.parent / "libxul.so"], [executable.parent]
    if engine["name"] == "webkit":
        bundle = executable.parent / "minibrowser-wpe"
        return [bundle / "bin/MiniBrowser"], [bundle / "lib", bundle / "sys/lib"]
    return [executable], []


def missing_libraries(config: Configuration, engines: list[dict], env: dict) -> list[str]:
    errors = []
    for engine in engines:
        files, bundled = headless_files(engine)
        local = browser_environment(config, env)
        local["LD_LIBRARY_PATH"] = os.pathsep.join([*(str(p) for p in bundled), local.get("LD_LIBRARY_PATH", "")])
        for path in files:
            result = subprocess.run(["ldd", str(path)], env=local, capture_output=True, text=True, timeout=30)
            # Also retain symbol-version failures, which a missing-soname-only check misses.
            lines = [line.strip() for line in (result.stdout + result.stderr).splitlines() if "not found" in line]
            if result.returncode and not lines:
                lines = [result.stderr.strip() or f"ldd exited {result.returncode}"]
            errors += [f"{engine['name']}: {line}" for line in lines]
        if engine["name"] == "webkit":
            result = subprocess.run([sys.executable, "-c", "import ctypes; ctypes.CDLL('libGLESv2.so.2')"],
                                    env=local, capture_output=True, text=True, timeout=30)
            if result.returncode:
                errors.append("webkit: libGLESv2.so.2 runtime load failed: " + result.stderr.strip())
    return sorted(set(errors))


def write_webkit_launcher(config: Configuration, engine: dict) -> Path:
    # The pinned Linux bundle's shell wrapper replaces LD_LIBRARY_PATH. Launch
    # its unchanged headless binary with both its bundled and cached host libs.
    files, bundled = headless_files(engine)
    binary = files[0]
    bundle = binary.parent.parent
    cached = browser_environment(config, {}).get("LD_LIBRARY_PATH", "")
    variables = {
        "WEBKIT_EXEC_PATH": str(bundle / "bin"),
        "WEBKIT_INJECTED_BUNDLE_PATH": str(bundle / "lib"),
        "WEBKIT_INSPECTOR_RESOURCES_PATH": str(bundle / "share"),
        "WEBKIT_FORCE_COMPLEX_TEXT": "1",
        "LD_LIBRARY_PATH": os.pathsep.join([*(str(p) for p in bundled), cached]),
    }
    host_root = config.deps / "browser-libs/root"
    vendor = host_root / "usr/share/glvnd/egl_vendor.d/50_mesa.json"
    drivers = sorted((host_root / "usr/lib").glob("*-linux-gnu/dri"))
    # EGL loads the vendor and DRI drivers at runtime, outside the ldd closure.
    if vendor.is_file():
        variables["__EGL_VENDOR_LIBRARY_FILENAMES"] = str(vendor)
    if drivers:
        variables["LIBGL_DRIVERS_PATH"] = os.pathsep.join(str(p) for p in drivers)
    script = "#!/bin/sh\n" + "".join(f"export {key}={shlex.quote(value)}\n" for key, value in variables.items())
    script += f"exec {shlex.quote(str(binary))} \"$@\"\n"
    path = config.deps / "browser-libs/launch-webkit.sh"
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(script)
    path.chmod(0o755)
    return path


def setup_browsers(config: Configuration) -> None:
    directory = config.deps / "receipts" / ("browser-matrix-" + run_id())
    directory.mkdir(parents=True)
    log = directory / "commands.log"
    report = {"kind": "postgamma.wasm-browser-setup", "status": "failed", "actions": []}
    try:
        tools, env = sdk_tools(config)
        info = description(config, tools, env)
        absent = [item["name"] for item in info["engines"] if not Path(item["executable"]).is_file()]
        if absent:
            run([tools["node"], str(config.package / "node_modules/playwright/cli.js"),
                 "install", *absent, "--no-shell"], log, env)
            report["actions"].append("install-" + ",".join(absent))
        report["browsers"] = info
        if platform.system() == "Linux":
            missing = missing_libraries(config, info["engines"], env)
            report["initial_missing_libraries"] = missing
            if missing:
                release = {}
                for line in Path("/etc/os-release").read_text().splitlines():
                    if "=" in line:
                        key, value = line.split("=", 1)
                        release[key] = value.strip('"')
                host = f"{release.get('ID')}-{release.get('VERSION_ID')}-{platform.machine()}"
                packages = config.profile.get("browser_matrix_host_packages", {}).get(host)
                if not packages:
                    raise WasmError("install Playwright browser dependencies for this host: " + "; ".join(missing))
                plan = capture(["apt-get", "--simulate", "--no-install-recommends", "--no-upgrade", "install", *packages],
                               dict(env, LC_ALL="C"))
                (directory / "apt-plan.txt").write_text(plan)
                requested = apt_download_plan(plan)
                if not requested:
                    raise WasmError("host library packages are installed but browser linking still fails: " + "; ".join(missing))
                downloads = config.deps / "browser-libs/downloads"
                downloads.mkdir(parents=True, exist_ok=True)
                print(f"wasm browser setup: caching {len(requested)} host packages", flush=True)
                run(["apt-get", "download", *requested], log, env, downloads)
                archives = sorted(downloads.glob("*.deb"))
                for archive in archives:
                    run(["dpkg-deb", "--extract", str(archive), str(config.deps / "browser-libs/root")], log, env)
                report["packages"] = requested
                report["archives"] = {p.name: digest(p) for p in archives}
                report["actions"].append("cache-host-libraries")
                remaining = missing_libraries(config, info["engines"], env)
                if remaining:
                    raise WasmError("browser libraries still unresolved: " + "; ".join(remaining))
            webkit = next(e for e in info["engines"] if e["name"] == "webkit")
            launcher = write_webkit_launcher(config, webkit)
            report["webkit_launcher"] = {"path": str(launcher), "sha256": digest(launcher)}
        report["status"] = "ready"
        write_json(config.deps / "receipts/browser-matrix.json", report)
        print("wasm browser setup: " + (", ".join(report["actions"]) or "reused browser matrix"), flush=True)
    except Exception as error:
        report["error"] = str(error)
        raise
    finally:
        write_json(directory / "report.json", report)
        print(f"wasm browser setup report: {directory / 'report.json'}", flush=True)
