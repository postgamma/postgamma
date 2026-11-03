"""Reject host configuration and objects in the target AST inputs."""

import copy
import json
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "buildsys"))
from wasm_kernel import clean_environment
from wasm_kernel_checks import configured_abi, validate_database
from wasm_toolchain import WasmError


class KernelCaptureTests(unittest.TestCase):
    def fixture(self, root):
        source, build, sdk = [root / name for name in ("postgres", "target", "sdk")]
        for path in (source, build, sdk):
            path.mkdir()
        compiler = sdk / "clang"
        compiler.touch()
        (source / "unit.c").write_text("int value;\n")
        (build / "unit.o").write_bytes(b"\0asm\x01\0\0\0")
        record = {"directory": str(build), "file": str(source / "unit.c"),
                  "arguments": [str(compiler), "-target", "wasm32-unknown-emscripten",
                                f"--sysroot={sdk}", "-pthread", "-I" + str(source),
                                "-c", str(source / "unit.c"), "-o", "unit.o"]}
        return source, build, sdk, compiler, record

    def test_native_headers_compilers_targets_and_objects_are_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            source, build, sdk, compiler, record = self.fixture(Path(temporary))
            database = build / "compile_commands.json"
            database.write_text(json.dumps([record]))
            self.assertEqual(validate_database(database, source, build, compiler, sdk)["commands"], 1)
            for change in (lambda a: a.append("-I/usr/include"),
                           lambda a: a.__setitem__(0, "/usr/bin/clang"),
                           lambda a: a.__setitem__(2, "x86_64-linux-gnu"),
                           lambda a: a.remove("-pthread")):
                changed = copy.deepcopy(record)
                change(changed["arguments"])
                database.write_text(json.dumps([changed]))
                with self.assertRaises(WasmError):
                    validate_database(database, source, build, compiler, sdk)
            database.write_text(json.dumps([record]))
            (build / "unit.o").write_bytes(b"\x7fELF\x02\x01\x01\x00")
            with self.assertRaisesRegex(WasmError, "not a WebAssembly"):
                validate_database(database, source, build, compiler, sdk)

    def test_host_cache_and_optional_library_flags_do_not_leak(self):
        env = clean_environment({"PATH": "/bin", "CC": "hostcc", "CFLAGS": "-mavx2",
                                 "CPATH": "/host", "ICU_LIBS": "-lhosticu",
                                 "ac_cv_sizeof_void_p": "8", "pgac_cv__128bit_int": "yes",
                                 "POSTGAMMA_CAPTURE_DIR": "/native"})
        for key in ("CC", "CFLAGS", "CPATH", "ICU_LIBS", "ac_cv_sizeof_void_p",
                    "pgac_cv__128bit_int", "POSTGAMMA_CAPTURE_DIR"):
            self.assertNotIn(key, env)
        self.assertEqual(env["CONFIG_SITE"], "/dev/null")
        self.assertEqual(env["PKG_CONFIG"], "false")

    def test_native_pointer_layout_is_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            header = Path(temporary) / "pg_config.h"
            header.write_text("#define SIZEOF_VOID_P 8\n")
            with self.assertRaisesRegex(WasmError, "SIZEOF_VOID_P"):
                configured_abi(header)

    def test_ineffective_presync_capability_is_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            header = Path(temporary) / "pg_config.h"
            layout = "\n".join(f"#define {name} {size}" for name, size in {
                "SIZEOF_VOID_P": 4, "SIZEOF_SIZE_T": 4, "SIZEOF_LONG": 4,
                "SIZEOF_LONG_LONG": 8, "SIZEOF_OFF_T": 8, "ALIGNOF_INT": 4,
                "ALIGNOF_DOUBLE": 8, "ALIGNOF_INT64_T": 8, "MAXIMUM_ALIGNOF": 8,
            }.items()) + "\n"
            header.write_text(layout)
            configured_abi(header)
            header.write_text(layout + "#define HAVE_POSIX_FADVISE 1\n")
            with self.assertRaisesRegex(WasmError, "ineffective pre-sync"):
                configured_abi(header)


if __name__ == "__main__":
    unittest.main()
