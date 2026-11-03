"""Host-compatible builds of the pinned Wasm tools, isolated under WASM_DEPS_DIR."""

from pathlib import Path
import platform
import shlex
import shutil
import tarfile
import urllib.error
import urllib.request
import zipfile

from wasm_toolchain import WasmError, capture, digest, read_json, run, write_json


def tool_paths(config) -> dict[str, Path]:
    """Derive tool selection from the host ABI; doctors never install anything."""
    if config.external_sdk or (platform.system(), platform.machine()) != ("Linux", "x86_64"):
        return {}
    libc, version = platform.libc_ver()
    if libc != "glibc" or not version:
        return {}
    version = tuple(int(part) for part in version.split(".")[:2])
    if version >= (2, 28):
        return {}
    pins = config.profile["legacy_linux_tools"]
    local = config.deps / "host-tools"
    tools = {}
    if version < (2, 28) and not config.node_override:
        tools["node"] = local / pins["node"]["directory"] / "bin/node"
    if version < (2, 18):
        tools["binaryen"] = local / ("binaryen-build-" + pins["binaryen"]["commit"])
        tools["llvm"] = local / ("llvm-glibc-" + pins["linker_runtime"]["version"]) / "bin"
    return tools


def download(pin: dict, cache: Path) -> Path:
    """Verify every archive before extraction, including previously cached downloads."""
    archive = cache / pin["filename"]
    cache.mkdir(parents=True, exist_ok=True)
    if archive.is_file() and digest(archive) == pin["sha256"]:
        return archive
    partial = archive.with_name(archive.name + ".partial")
    try:
        for attempt in range(3):
            try:
                with urllib.request.urlopen(pin["url"], timeout=60) as source, partial.open("wb") as target:
                    shutil.copyfileobj(source, target)
                break
            except (OSError, urllib.error.URLError):
                if attempt == 2:
                    raise
        if digest(partial) != pin["sha256"]:
            raise WasmError("Wasm host tool archive checksum mismatch: " + str(archive))
        partial.replace(archive)
    finally:
        partial.unlink(missing_ok=True)
    return archive


def cmake_tool(config) -> Path:
    # Both Binaryen builds and .conda extraction use this private CMake.
    pin = config.profile["legacy_linux_tools"]["cmake"]
    local = config.deps / "host-tools"
    archive = download(pin, local / "downloads")
    root = local / ("cmake-" + pin["version"])
    with zipfile.ZipFile(archive) as source:
        source.extractall(root)
    cmake = root / "cmake/data/bin/cmake"
    cmake.chmod(0o755)
    return cmake


def linker_identity(config) -> dict:
    """Check the private runtime without installing or repairing it in a doctor."""
    llvm = tool_paths(config).get("llvm")
    if llvm is None:
        return {}
    root = llvm.parent
    pin = config.profile["legacy_linux_tools"]["linker_runtime"]
    receipt = root / "postgamma-tools.json"
    if not receipt.is_file():
        raise WasmError("Wasm linker runtime is missing; run make wasm-setup")
    state = read_json(receipt)
    files = state.get("files", {})
    lib = "runtime/" + pin["library_directory"]
    required = {"bin/wasm-ld", lib + "/ld-linux-x86-64.so.2", lib + "/libc.so.6"}
    original = config.sdk / "upstream/bin/wasm-ld"
    if (state.get("pin") != pin or not required.issubset(files) or
            state.get("sdk_wasm_ld_sha256") != digest(original) or
            not all((root / p).is_file() and digest(root / p) == sha for p, sha in files.items())):
        raise WasmError("Wasm linker runtime identity mismatch; run make wasm-setup")
    for tool in original.parent.iterdir():
        if tool.name != "wasm-ld" and (not (llvm / tool.name).is_symlink() or
                                       (llvm / tool.name).resolve() != tool.resolve()):
            raise WasmError("Wasm LLVM tool overlay mismatch; run make wasm-setup")
    return state


def setup_linker(config, log: Path) -> None:
    """Run the unchanged SDK linker with its own loader on pre-2.18 glibc hosts."""
    llvm = tool_paths(config).get("llvm")
    original = config.sdk / "upstream/bin/wasm-ld"
    if llvm is None or not original.is_file():
        return  # A fresh SDK installation is prepared before the next call.
    try:
        linker_identity(config)
        capture([str(llvm / "wasm-ld"), "--version"])
        return
    except (WasmError, OSError, ValueError):
        pass
    print("wasm setup: isolating the SDK linker with a checksum-pinned glibc runtime", flush=True)
    root = llvm.parent
    pin = config.profile["legacy_linux_tools"]["linker_runtime"]
    archive = download(pin, config.deps / "host-tools/downloads")
    runtime = root / "runtime"
    runtime.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(archive) as source:
        parts = [p for p in source.namelist() if p.startswith("pkg-") and p.endswith(".tar.zst")]
        if len(parts) != 1:
            raise WasmError("invalid linker runtime package payload")
        payload = Path(source.extract(parts[0], runtime))
    run([str(cmake_tool(config)), "-E", "tar", "xf", str(payload)], log, cwd=runtime)
    payload.unlink()
    lib = runtime / pin["library_directory"]
    llvm.mkdir(parents=True, exist_ok=True)
    for tool in original.parent.iterdir():
        target = llvm / tool.name
        target.unlink(missing_ok=True)
        if tool.name == "wasm-ld":
            # Do not resolve wasm-ld to lld: argv[0] selects the WebAssembly driver.
            command = [lib / "ld-linux-x86-64.so.2", "--library-path", lib, original]
            target.write_text('#!/bin/sh\nexec ' + ' '.join(shlex.quote(str(p)) for p in command) + ' "$@"\n')
            target.chmod(0o755)
        else:
            target.symlink_to(tool)
    version = capture([str(llvm / "wasm-ld"), "--version"])
    files = {str(p.relative_to(root)): digest(p) for p in sorted(lib.iterdir()) if p.is_file()}
    files["bin/wasm-ld"] = digest(llvm / "wasm-ld")
    write_json(root / "postgamma-tools.json", {"pin": pin, "version": version,
               "sdk_wasm_ld_sha256": digest(original), "files": files})
    linker_identity(config)


def setup(config, log: Path) -> None:
    paths = tool_paths(config)
    if not paths:
        return
    pins = config.profile["legacy_linux_tools"]
    local = config.deps / "host-tools"
    if "node" in paths:
        pin, node = pins["node"], paths["node"]
        if not node.is_file() or digest(node) != pin["binary_sha256"]:
            print("wasm setup: installing checksum-pinned Node " + pin["version"] + " for glibc 2.17", flush=True)
            archive = download(pin, local / "downloads")
            with tarfile.open(archive) as source:
                source.extractall(local, filter="data")
        if digest(node) != pin["binary_sha256"] or capture([str(node), "--version"]) != pin["version"]:
            raise WasmError("compatible Node build identity mismatch")
        write_json(local / "node.json", {"pin": pin, "binary_sha256": digest(node)})
    if "binaryen" in paths:
        pin, build = pins["binaryen"], paths["binaryen"]
        receipt = build / "postgamma-tools.json"
        if receipt.is_file():
            state = read_json(receipt)
            files = state.get("files", {})
            if (state.get("pin") == pin and files and
                    all((build / p).is_file() and digest(build / p) == sha for p, sha in files.items())):
                capture([str(build / "bin/wasm-opt"), "--version"])
                return
        print("wasm setup: building the emsdk-pinned Binaryen against the host-compatible toolchain", flush=True)
        archive = download(pin, local / "downloads")
        source_dir = local / ("binaryen-" + pin["commit"])
        with tarfile.open(archive) as source:
            source.extractall(local, filter="data")
        cmake = cmake_tool(config)
        run([str(cmake), "-S", str(source_dir), "-B", str(build), "-G", "Unix Makefiles",
             "-DCMAKE_BUILD_TYPE=Release", "-DBUILD_SHARED_LIBS=OFF", "-DBUILD_TESTS=OFF",
             "-DBUILD_EMSCRIPTEN_TOOLS_ONLY=ON"], log)
        # Source-building the optimizer is a setup step, not a test timeout change.
        run([str(cmake), "--build", str(build), "--parallel", str(config.jobs)], log, timeout=7200)
        version = capture([str(build / "bin/wasm-opt"), "--version"])
        files = {str(p.relative_to(build)): digest(p) for p in sorted((build / "bin").iterdir()) if p.is_file()}
        write_json(receipt, {"pin": pin, "version": version, "files": files})
