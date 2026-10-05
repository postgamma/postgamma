"""Compile the initdb redirection header against the host libc declarations."""

from __future__ import annotations

import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parents[2]


class InitdbRuntimeHeaderTests(unittest.TestCase):
    def test_libc_declarations_precede_geteuid_redirection(self) -> None:
        compiler = shutil.which("cc")
        if compiler is None:
            self.skipTest("C compiler is unavailable")
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            private = directory / "postgamma/private"
            private.mkdir(parents=True)
            (directory / "postgres_fe.h").write_text(
                "#ifndef POSTGRES_FE_H\n"
                "#define POSTGRES_FE_H\n"
                "#include <stdbool.h>\n"
                "#include <stdint.h>\n"
                "#include <sys/types.h>\n"
                "#define pg_noreturn __attribute__((noreturn))\n"
                "#define pg_attribute_printf(f, a) "
                "__attribute__((format(printf, f, a)))\n"
                "typedef void (*pqsigfunc)(int);\n"
                "#endif\n",
                encoding="ascii",
            )
            (private / "initdb_host.h").write_text(
                "typedef enum PostgammaInitdbPhase {\n"
                "    POSTGAMMA_INITDB_PHASE_TEST\n"
                "} PostgammaInitdbPhase;\n",
                encoding="ascii",
            )
            (directory / "port.h").write_text("/* Test stub. */\n", encoding="ascii")
            source = directory / "probe.c"
            source.write_text(
                "#define POSTGAMMA_INITDB_UPSTREAM 1\n"
                "#include \"postgamma/private/initdb_runtime.h\"\n"
                "#include <unistd.h>\n"
                "uid_t probe(void) { return geteuid(); }\n",
                encoding="ascii",
            )
            result = subprocess.run(
                [
                    compiler,
                    "-std=c11",
                    "-Wall",
                    "-Werror",
                    f"-I{directory}",
                    f"-I{PROJECT_ROOT / 'embedded-c/include'}",
                    "-fsyntax-only",
                    str(source),
                ],
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                check=False,
            )
            self.assertEqual(result.returncode, 0, result.stdout)


if __name__ == "__main__":
    unittest.main()
