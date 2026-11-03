"""Check target compilation provenance before AST tools consume it."""

from pathlib import Path
import re

from cc_capture import output_argument
from wasm_toolchain import WasmError, digest, read_json


def include_paths(arguments: list[str], cwd: Path) -> list[Path]:
    result = []
    index = 0
    while index < len(arguments):
        value = arguments[index]
        if value in ("-I", "-isystem", "-iquote", "-idirafter", "-include"):
            index += 1
            if index == len(arguments):
                raise WasmError("missing include argument")
            value = arguments[index]
        elif value.startswith("-I"):
            value = value[2:]
        else:
            index += 1
            continue
        path = Path(value)
        result.append((cwd / path).resolve())
        index += 1
    return result


def validate_database(database: Path, source: Path, build: Path,
                      compiler: Path, sysroot: Path) -> dict:
    commands = read_json(database)
    if not isinstance(commands, list) or not commands:
        raise WasmError("target compile database is empty")
    outputs = {}
    translation_units = set()
    roots = (source.resolve(), build.resolve(), sysroot.resolve())
    for record in commands:
        args = record["arguments"]
        cwd = Path(record["directory"]).resolve()
        if Path(args[0]).resolve() != compiler.resolve():
            raise WasmError("compile database contains a different compiler")
        if not any(args[i:i + 2] == ["-target", "wasm32-unknown-emscripten"]
                   for i in range(len(args) - 1)):
            raise WasmError("compile database is missing the selected Wasm target")
        if f"--sysroot={sysroot}" not in args or "-pthread" not in args:
            raise WasmError("compile database is missing the selected sysroot/pthreads")
        if not cwd.is_relative_to(build.resolve()):
            raise WasmError("compile directory is outside the target build")
        for path in [Path(record["file"]).resolve(), *include_paths(args, cwd)]:
            if not any(path == root or path.is_relative_to(root) for root in roots):
                raise WasmError(f"compile database references an undeclared input: {path}")
        output = output_argument(args, cwd)
        if output is None or not output.is_relative_to(build.resolve()):
            raise WasmError("target object is outside the target build")
        with output.open("rb") as stream:
            if stream.read(8) != b"\0asm\x01\0\0\0":
                raise WasmError(f"object is not a WebAssembly object: {output}")
        outputs[str(output)] = digest(output)
        translation_units.add(record["file"])
    return {"commands": len(commands), "translation_units": len(translation_units),
            "database_sha256": digest(database), "objects": outputs}


def configured_abi(header: Path) -> dict:
    expected = {"SIZEOF_VOID_P": 4, "SIZEOF_SIZE_T": 4, "SIZEOF_LONG": 4,
                "SIZEOF_LONG_LONG": 8, "SIZEOF_OFF_T": 8,
                "ALIGNOF_INT": 4, "ALIGNOF_DOUBLE": 8,
                "ALIGNOF_INT64_T": 8, "MAXIMUM_ALIGNOF": 8}
    text = header.read_text()
    actual = {}
    for name, value in expected.items():
        match = re.search(r"^#define " + name + r" (\d+)$", text, re.M)
        if not match or int(match[1]) != value:
            raise WasmError(f"target configure ABI differs from W03: {name}")
        actual[name] = int(match[1])
    # Emscripten's advisory syscall is a no-op. Advertising it makes startup
    # scan every database file before the real fsync pass without any writeback.
    if re.search(r"^#define HAVE_POSIX_FADVISE\b", text, re.M):
        raise WasmError("target advertises an ineffective pre-sync hint: HAVE_POSIX_FADVISE")
    return actual
