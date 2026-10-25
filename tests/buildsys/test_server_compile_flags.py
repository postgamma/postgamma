"""Check server-only inlining compensation and caller/compiler boundaries."""

import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class ServerCompileFlagsTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.work = Path(temporary.name)
        (self.work / "src/backend").mkdir(parents=True)
        adapter = json.loads((ROOT / "manifests/postgresql/adapter.json").read_text())
        copy = next(item for item in adapter["generated_support"]["copies"]
                    if item["id"] == "runtime.server-compile-flags")
        shutil.copyfile(ROOT / copy["source"], self.work / copy["target"])
        domain = next(item["content"] for item in adapter["generated_support"]["edits"]
                      if item["id"] == "build.mark-backend-domain")
        (self.work / "src/backend/common.mk").write_text(domain)
        (self.work / "Makefile").write_text(
            "top_srcdir := .\n"
            "include src/backend/common.mk\n"
            "$(file >flags.txt,$(strip $(CFLAGS)))\n"
            "all:\n\t@:\n"
        )
        self.compiler = self.work / "compiler"
        self.compiler.write_text(
            "#!/bin/sh\n"
            "printf 'probe\\n' >> probes.txt\n"
            'printf "%s\\n" "${TEST_COMPILER_PARAMS-}"\n'
        )
        self.compiler.chmod(0o755)

    def flags(self, cflags, direct="1", compiler=None, budget="15"):
        env = dict(os.environ, TEST_COMPILER_PARAMS=(
            f"  --param=max-inline-insns-auto= {budget}"))
        for key in ("MAKEFLAGS", "MFLAGS", "MAKELEVEL", "MAKEOVERRIDES"):
            env.pop(key, None)
        result = subprocess.run(
            [*shlex.split(os.environ.get("MAKE", "make")), "--no-print-directory",
             f"CC={compiler or self.compiler}", f"CFLAGS={cflags}",
             f"POSTGAMMA_DIRECT_ROLE_STATE={direct}"],
            cwd=self.work, env=env, capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return shlex.split((self.work / "flags.txt").read_text())

    def test_direct_o2_compensation_preserves_other_flags(self):
        self.assertEqual(self.flags("-g -O2 -fPIC -fwrapv"),
                         ["-g", "-O2", "-fPIC", "-fwrapv",
                          "--param=max-inline-insns-auto=20"])
        self.assertEqual((self.work / "probes.txt").read_text(), "probe\n")

    def test_other_optimization_levels_and_default_access_do_not_probe(self):
        for options in ("", "-O0", "-O1", "-O3", "-Os", "-Oz", "-Og", "-Ofast",
                        "-O2 -O0", "-O2 -O3"):
            with self.subTest(options=options):
                self.assertEqual(self.flags(options), shlex.split(options))
        self.assertEqual(self.flags("-O2", direct="0"), ["-O2"])
        self.assertFalse((self.work / "probes.txt").exists())

    def test_explicit_inline_tuning_takes_precedence(self):
        for tuning in ("--param=max-inline-insns-auto=5",
                       "--param max-inline-insns-auto=32", "-finline-limit=10"):
            with self.subTest(tuning=tuning):
                self.assertEqual(self.flags("-O2 " + tuning),
                                 shlex.split("-O2 " + tuning))
        self.assertFalse((self.work / "probes.txt").exists())

    def test_larger_or_unrecognized_compiler_budget_is_unchanged(self):
        for budget in ("20", "30", "100", "", "unavailable"):
            with self.subTest(budget=budget):
                self.assertEqual(self.flags("-O2", budget=budget), ["-O2"])

    def test_real_compilers_accept_selected_flags(self):
        compilers = [shlex.split(os.environ.get("CC", "cc"))]
        clang = shutil.which("clang") or shutil.which("clang-22")
        if clang:
            compilers.append([clang])
        for compiler in compilers:
            with self.subTest(compiler=compiler):
                flags = self.flags("-O2 -Werror", compiler=shlex.join(compiler))
                subprocess.run([*compiler, *flags, "-x", "c", "-c", "-",
                                "-o", str(self.work / "probe.o")],
                               input="int probe(int x) { return x + 1; }\n",
                               capture_output=True, text=True, check=True)
                if compiler[0] == clang:
                    self.assertEqual(flags, ["-O2", "-Werror"])


if __name__ == "__main__":
    unittest.main()
