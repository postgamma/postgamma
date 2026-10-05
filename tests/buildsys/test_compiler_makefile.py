"""Exercise the private compiler toolchain Makefile contract."""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parents[2]
COMPILER_DIR = PROJECT_ROOT / "compiler"


class CompilerMakefileTests(unittest.TestCase):
    def setUp(self) -> None:
        self.make = shutil.which("make")
        if self.make is None:
            self.skipTest("GNU Make is unavailable")

    def run_make(
        self, *arguments: str, environment: dict[str, str] | None = None
    ) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [self.make, "--no-print-directory", "-C", str(COMPILER_DIR), *arguments],
            env=environment,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            check=False,
        )

    def test_explicit_python_controls_tool_discovery(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            trace = directory / "python-invocations"
            wrapper = directory / "selected-python"
            wrapper.write_text(
                "#!/bin/sh\n"
                "printf '%s\\n' \"$*\" >>\"$TRACE_FILE\"\n"
                "case \"$2\" in\n"
                "  llvm-config) printf '%s\\n' /mock/llvm-config ;;\n"
                "  clang++) printf '%s\\n' /mock/clang++ ;;\n"
                "  clang-cpp-library) printf '%s\\n' /mock/libclang-cpp.so ;;\n"
                "  clang-resource-dir) printf '%s\\n' /mock/clang-resource ;;\n"
                "  *) exit 9 ;;\n"
                "esac\n",
                encoding="ascii",
            )
            wrapper.chmod(0o755)
            makefile = directory / "print-tools.mk"
            makefile.write_text(
                f"include {COMPILER_DIR / 'Makefile'}\n"
                "print-tools:\n"
                "\t@printf '%s\\n' "
                "'$(LLVM_CONFIG)|$(CLANGXX)|$(CLANG_CPP_LIBRARY)|$(CLANG_RESOURCE_DIR)'\n",
                encoding="ascii",
            )
            environment = dict(os.environ)
            for variable in (
                "LLVM_CONFIG",
                "CLANGXX",
                "CLANG_CPP_LIBRARY",
                "CLANG_RESOURCE_DIR",
            ):
                environment.pop(variable, None)
            environment["TRACE_FILE"] = str(trace)
            result = self.run_make(
                "-f", str(makefile), "print-tools", f"PYTHON={wrapper}",
                environment=environment,
            )
            self.assertEqual(result.returncode, 0, result.stdout)
            self.assertIn(
                "/mock/llvm-config|/mock/clang++|/mock/libclang-cpp.so|"
                "/mock/clang-resource",
                result.stdout,
            )
            invocations = trace.read_text(encoding="ascii").splitlines()
            self.assertEqual(len(invocations), 4)
            self.assertEqual(
                {arguments.split()[1] for arguments in invocations},
                {"llvm-config", "clang++", "clang-cpp-library", "clang-resource-dir"},
            )

    def test_missing_discovered_tool_fails_before_compilation(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            build = Path(temporary) / "build"
            result = self.run_make(
                str(build / "postgamma_cc.o"),
                f"BUILD_DIR={build}",
                "LLVM_CONFIG=",
                "CLANGXX=",
                "CLANG_CPP_LIBRARY=",
                "CLANG_RESOURCE_DIR=",
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertIn(
                "compiler: llvm-config discovery failed; run make doctor-ast",
                result.stdout,
            )
            self.assertFalse((build / "postgamma_cc.o").exists())

    def test_compile_capture_uses_the_selected_python(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            fake_bin = directory / "bin"
            fake_bin.mkdir()
            default_python_marker = directory / "default-python-used"
            (fake_bin / "python3").write_text(
                "#!/bin/sh\n"
                f"touch {default_python_marker}\n"
                "exit 97\n",
                encoding="ascii",
            )
            (fake_bin / "python3").chmod(0o755)
            compiler_marker = directory / "compiler-used"
            real_compiler = directory / "real-compiler"
            real_compiler.write_text(
                "#!/bin/sh\n"
                f"printf '%s\\n' \"$*\" >{compiler_marker}\n",
                encoding="ascii",
            )
            real_compiler.chmod(0o755)
            nested_makefile = directory / "nested.mk"
            nested_makefile.write_text(
                "all:\n"
                "\t@$(CC) -DVAL_CC='\"$(CC)\"' -c selected-python-probe.c\n",
                encoding="ascii",
            )
            environment = dict(os.environ)
            environment["PATH"] = f"{fake_bin}:{environment['PATH']}"
            print_rule = (
                "print-postgamma-cc-capture:\n"
                "\t@printf '%s\\n' '$(POSTGAMMA_CC_CAPTURE)'\n"
            )
            result = subprocess.run(
                [
                    self.make,
                    "--no-print-directory",
                    "-C",
                    str(PROJECT_ROOT),
                    "--eval",
                    print_rule,
                    "print-postgamma-cc-capture",
                    f"PYTHON={sys.executable}",
                ],
                env=environment,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                check=False,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            capture_command = result.stdout.strip()
            self.assertEqual(
                capture_command,
                f"{sys.executable} {PROJECT_ROOT / 'buildsys/cc_capture.py'}",
            )
            environment["POSTGAMMA_REAL_CC"] = str(real_compiler)
            result = subprocess.run(
                [
                    self.make,
                    "--no-print-directory",
                    "-f",
                    str(nested_makefile),
                    f"CC={capture_command}",
                ],
                cwd=directory,
                env=environment,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                check=False,
            )
            self.assertEqual(result.returncode, 0, result.stdout)
            self.assertTrue(compiler_marker.is_file())
            self.assertFalse(default_python_marker.exists())
            compiler_arguments = compiler_marker.read_text(encoding="ascii")
            self.assertIn(f'-DVAL_CC="{real_compiler}"', compiler_arguments)
            self.assertNotIn("cc_capture.py", compiler_arguments)
            makefile = (PROJECT_ROOT / "Makefile").read_text(encoding="utf-8")
            self.assertNotIn('CC="cc_capture.py"', makefile)
            self.assertEqual(
                makefile.count('CC="$(POSTGAMMA_CC_CAPTURE)"'),
                3,
            )


if __name__ == "__main__":
    unittest.main()
