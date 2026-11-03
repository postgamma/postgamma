"""Private browser host libraries for old Linux hosts; never install OS packages.

The SDK configuration owns selection and WASM_DEPS_DIR owns lifetime. Reuse
host-tool download/extraction and the existing browser doctors. Only executable
ELF loader metadata changes in a copy; browser code and shared libraries do not.
"""

from concurrent.futures import ThreadPoolExecutor
import hashlib
import os
from pathlib import Path
import platform
import shlex
import shutil
import struct
import tempfile
import zipfile

from wasm_toolchain import WasmError, digest, read_json, run, write_json


def location(config) -> Path | None:
    pin = config.profile.get("legacy_linux_tools", {}).get("browser_runtime")
    if (not pin or config.external_sdk or config.browser_override or
            (platform.system(), platform.machine()) != ("Linux", "x86_64")):
        return None
    libc, version = platform.libc_ver()
    if libc != "glibc" or not version or tuple(map(int, version.split(".")[:2])) >= (2, 35):
        return None
    return config.deps / "browser-runtime-ubuntu22"


def library_paths(local: Path) -> list[Path]:
    return [local / "root" / p for p in
            ("usr/lib/x86_64-linux-gnu", "lib/x86_64-linux-gnu", "usr/lib")]


def environment(config, env: dict, *, install: bool = False) -> dict:
    result = dict(env)
    local = location(config)
    if local is not None:
        result["PLAYWRIGHT_HOST_PLATFORM_OVERRIDE"] = "ubuntu22.04-x64"
        result["PLAYWRIGHT_BROWSERS_PATH"] = str(local / ("raw-browsers" if install else "browsers"))
        if not install:
            result["PATH"] = str(local / "bin") + os.pathsep + result.get("PATH", os.defpath)
            result["FONTCONFIG_FILE"] = str(local / "fonts.conf")
    # Private glibc must never enter Node, Python or native compilers via LD_LIBRARY_PATH.
    return result


def elf_metadata(path: Path) -> tuple[bool, str]:
    """Find PT_INTERP and hash executable sections, independent of file offsets."""
    with path.open("rb") as stream:
        header = stream.read(64)
        if header[:6] != b"\x7fELF\x02\x01":
            return False, ""
        phoff, shoff = struct.unpack_from("<QQ", header, 32)
        phsize, phnum, shsize, shnum = struct.unpack_from("<HHHH", header, 54)
        stream.seek(phoff)
        programs = stream.read(phsize * phnum)
        interp = any(struct.unpack_from("<I", programs, i * phsize)[0] == 3 for i in range(phnum))
        if not interp:
            return False, ""  # Never rewrite shared libraries or static tools.
        stream.seek(shoff)
        sections = stream.read(shsize * shnum)
        code = hashlib.sha256()
        for i in range(shnum):
            _, kind, flags, _, offset, size = struct.unpack_from("<IIQQQQ", sections, i * shsize)
            if flags & 4 and kind != 8:  # SHF_EXECINSTR, excluding SHT_NOBITS.
                stream.seek(offset)
                code.update(stream.read(size))
        return True, code.hexdigest()


def identity(config) -> dict:
    """Check cached identities without repairing anything in a doctor."""
    local = location(config)
    if local is None:
        return {}
    receipt = local / "receipt.json"
    if not receipt.is_file():
        raise WasmError("private browser runtime missing; run make wasm-browser-setup")
    state = read_json(receipt)
    pin = config.profile["legacy_linux_tools"]["browser_runtime"]
    files = state.get("files", {})
    required = {"root/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2",
                "root/lib/x86_64-linux-gnu/libc.so.6", "bin/ldd", "fonts.conf"}
    if (state.get("manifest_sha256") != pin["sha256"] or
            digest(config.root / pin["manifest"]) != pin["sha256"] or
            state.get("package_lock_sha256") != digest(config.package / "package-lock.json") or
            state.get("browsers_json_sha256") != digest(config.package / "node_modules/playwright-core/browsers.json") or
            not required.issubset(files) or not state.get("executables") or
            not all((local / p).is_file() and digest(local / p) == sha for p, sha in files.items())):
        raise WasmError("private browser runtime identity mismatch; run make wasm-browser-setup")
    return state


def copy_executables(raw, browsers, originals, local, patcher, log):
    """Reuse the same loader adaptation and unchanged-code check for both browser sources."""
    if browsers.exists():
        shutil.rmtree(browsers)
    shutil.copytree(raw, browsers, symlinks=True)
    loader = local / "root/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2"
    patched = []
    for name, original in originals.items():
        path = browsers / name
        if path.is_symlink():
            continue
        interp, code = elf_metadata(path)
        if not interp:
            continue
        paths = [path.parent, path.parent.parent / "lib", path.parent.parent / "sys/lib", *library_paths(local)]
        run([str(patcher), "--force-rpath", "--set-rpath",
             ":".join(str(p) for p in paths if p.is_dir()), "--set-interpreter", str(loader), str(path)], log)
        if elf_metadata(path) != (True, code):
            raise WasmError("browser executable code changed: " + name)
        patched.append({"file": name, "original_sha256": original,
                        "sha256": digest(path), "code_sha256": code})
    changed = {row["file"] for row in patched}
    for name, original in originals.items():
        if digest(raw / name) != original or (name not in changed and digest(browsers / name) != original):
            raise WasmError("browser payload unexpectedly changed: " + name)
    return patched


def setup(config, log: Path) -> dict:
    local = location(config)
    if local is None:
        return {}
    from wasm_host_tools import cmake_tool, download
    pin = config.profile["legacy_linux_tools"]["browser_runtime"]
    manifest = config.root / pin["manifest"]
    if digest(manifest) != pin["sha256"]:
        raise WasmError("browser runtime manifest checksum mismatch")
    lock = read_json(manifest)
    raw = local / "raw-browsers"
    # Playwright owns its markers and .links. Record only installed payloads.
    originals = {str(p.relative_to(raw)): digest(p) for directory in sorted(raw.glob("*-*"))
                 if directory.is_dir() for p in sorted(directory.rglob("*"))
                 if p.is_file() and p.name not in {"DEPENDENCIES_VALIDATED", "INSTALLATION_COMPLETE"}}
    if not originals:
        raise WasmError("install the locked browsers before preparing their runtime")
    try:
        previous = identity(config)
        if previous.get("originals") == originals:
            return previous
    except (WasmError, OSError, ValueError):
        pass
    print("wasm setup: preparing checksum-pinned private browser libraries", flush=True)
    cache = local / "downloads"
    with ThreadPoolExecutor(max_workers=4) as pool:
        archives = list(pool.map(lambda package: download(package, cache), lock["packages"]))
    cmake = cmake_tool(config)
    runtime = local / "root"
    runtime.mkdir(parents=True, exist_ok=True)
    for archive in archives:
        # CMake's libarchive reads both the .deb ar container and its data.tar.*.
        with tempfile.TemporaryDirectory(prefix="unpack-", dir=local) as temporary:
            run([str(cmake), "-E", "tar", "xf", str(archive)], log, cwd=Path(temporary))
            payload = list(Path(temporary).glob("data.tar.*"))
            if len(payload) != 1:
                raise WasmError("invalid browser library package: " + str(archive))
            run([str(cmake), "-E", "tar", "xf", str(payload[0])], log, cwd=runtime)
    archive = download(lock["patchelf"], cache)
    with zipfile.ZipFile(archive) as source:
        entries = [p for p in source.namelist() if p.endswith(".data/scripts/patchelf")]
        if len(entries) != 1:
            raise WasmError("invalid patchelf host tool archive")
        patcher = Path(source.extract(entries[0], local / "tools"))
        patcher.chmod(0o755)
    browsers = local / "browsers"
    patched = copy_executables(raw, browsers, originals, local, patcher, log)
    loader = runtime / "lib/x86_64-linux-gnu/ld-linux-x86-64.so.2"
    # Playwright scans libxul as well as executables. Use the same real loader
    # and search path for its ldd checks; do not skip host validation.
    check = local / "bin/ldd"
    check.parent.mkdir(parents=True, exist_ok=True)
    paths = ":".join(map(str, library_paths(local)))
    check.write_text('#!/bin/sh\ncase "$1" in\n' + shlex.quote(str(browsers)) + '/*)\n'
                     '  exec ' + shlex.quote(str(loader)) +
                     ' --library-path "${1%/*}:${1%/*}/../lib:${1%/*}/../sys/lib:' + paths + '" --list "$1" ;;\n'
                     '*) exec /usr/bin/ldd "$@" ;;\nesac\n')
    check.chmod(0o755)
    from xml.sax.saxutils import escape
    (local / "fonts.conf").write_text('<?xml version="1.0"?><!DOCTYPE fontconfig SYSTEM "fonts.dtd">\n'
        '<fontconfig><dir>' + escape(str(runtime / "usr/share/fonts")) + '</dir><cachedir>' +
        escape(str(local / "font-cache")) + '</cachedir></fontconfig>\n')
    files = {str(p.relative_to(local)): digest(p) for p in sorted(runtime.rglob("*"))
             if p.is_file() and not p.is_symlink()}
    files.update({"browsers/" + name: digest(browsers / name) for name in originals})
    files.update({str(p.relative_to(local)): digest(p) for p in (loader, check, local / "fonts.conf")})
    state = {"kind": "postgamma.private-browser-runtime", "manifest_sha256": pin["sha256"],
             "package_lock_sha256": digest(config.package / "package-lock.json"),
             "browsers_json_sha256": digest(config.package / "node_modules/playwright-core/browsers.json"),
             "host_platform": lock["host_platform"], "originals": originals, "executables": patched,
             "files": files, "system_packages_changed": False}
    write_json(local / "receipt.json", state)
    identity(config)
    return state


def prepare_firefox(config, executable: Path, receipt: Path, log: Path):
    """Adapt a verified native Mozilla bundle, retaining its original tree and receipt."""
    local = location(config)
    if local is None:
        return executable, None
    identity(config)
    upstream = read_json(receipt)
    raw = receipt.parent / 'bundle'
    directory = local / 'native-firefox' / upstream['pin']['version']
    target, record = directory / 'bundle', directory / 'receipt.json'
    inputs = {'upstream_receipt_sha256': digest(receipt),
              'runtime_receipt_sha256': digest(local / 'receipt.json')}
    originals = dict(upstream['files'])
    policy = upstream['configuration']
    originals[policy['path']] = policy['sha256']
    if any(digest(raw / name) != sha for name, sha in originals.items()):
        raise WasmError('Native Firefox upstream files changed')
    if record.is_file():
        state = read_json(record)
        if (state.get('inputs') != inputs or state.get('originals') != originals or
                set(state.get('files', {})) != set(originals) or
                any(not (target / name).is_file() or digest(target / name) != sha
                    for name, sha in state['files'].items())):
            raise WasmError('Private native Firefox identity mismatch')
    else:
        patchers = list((local / 'tools').glob('*.data/scripts/patchelf'))
        if len(patchers) != 1:
            raise WasmError('Verified browser runtime patcher is unavailable')
        directory.mkdir(parents=True, exist_ok=True)
        patched = copy_executables(raw, target, originals, local, patchers[0], log)
        write_json(record, {'kind': 'postgamma.private-native-firefox', 'inputs': inputs,
            'originals': originals, 'executables': patched,
            'files': {name: digest(target / name) for name in originals},
            'scope': 'Pinned Mozilla code and shared libraries unchanged; private ELF loader metadata only.'})
    return target / executable.relative_to(raw), record
