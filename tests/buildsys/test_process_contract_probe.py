"""Compile the process-contract probe against the supported glibc floor."""

from __future__ import annotations

import platform
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parents[2]


@unittest.skipUnless(
    platform.system() == "Linux" and platform.libc_ver()[0] == "glibc",
    "the process-contract probe requires Linux with glibc",
)
class ProcessContractProbeTests(unittest.TestCase):
    def test_compiles_without_mallinfo2_on_glibc_2_32(self) -> None:
        compiler = shutil.which("cc")
        if compiler is None:
            self.skipTest("C compiler is unavailable")

        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            compatibility_header = directory / "glibc-2.32.h"
            compatibility_header.write_text(
                "#include <features.h>\n"
                "#if !defined(__GLIBC__)\n"
                '#error "glibc is required"\n'
                "#endif\n"
                "#undef __GLIBC_MINOR__\n"
                "#define __GLIBC_MINOR__ 32\n",
                encoding="ascii",
            )
            result = subprocess.run(
                [
                    compiler,
                    "-std=gnu11",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-Wpedantic",
                    "-pthread",
                    "-include",
                    str(compatibility_header),
                    f"-I{PROJECT_ROOT}",
                    f"-I{PROJECT_ROOT / 'runtime/include'}",
                    f"-I{PROJECT_ROOT / 'embedded-c/include'}",
                    "-c",
                    str(
                        PROJECT_ROOT
                        / "tests/embedded/kernel/process_contract_probe.c"
                    ),
                    "-o",
                    str(directory / "process_contract_probe.o"),
                ],
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                check=False,
            )
            self.assertEqual(result.returncode, 0, result.stdout)


if __name__ == "__main__":
    unittest.main()
