"""Check real optimized accesses, dynamic fallbacks and mixed-set linkage."""
import copy
import json
import os
from pathlib import Path
import platform
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

from tests.buildsys import test_state_preparation_registry as registry
from generate_backend_state_preparation import emit, emit_direct
from plan_backend_state_preparation import digest
from qualify_backend_state_preparation import preparation_set_identity

ROOT = Path(__file__).resolve().parents[2]


class DirectStateAccessTests(unittest.TestCase):
    def compilers(self):
        commands = []
        if "CC" in os.environ:
            command = shlex.split(os.environ["CC"])
            self.assertTrue(command, "CC must contain a compiler command")
            commands.append(command)
        elif shutil.which("cc"):
            commands.append(["cc"])
        if shutil.which("clang") and ["clang"] not in commands:
            commands.append(["clang"])
        if not commands:
            self.skipTest("no C compiler")
        return commands

    def run_command(self, command, *, check=True):
        result = subprocess.run(
            command, capture_output=True, text=True, timeout=60,
        )
        if check:
            self.assertEqual(
                result.returncode, 0,
                f"command failed ({result.returncode}): {shlex.join(command)}\n"
                f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}",
            )
        return result

    def test_volatile_accesses_are_not_folded_or_cached_as_values(self):
        source = r'''
#include "postgamma/backend_state_runtime.h"
static volatile int Value;
int read_twice(void) {
    volatile int *pointer = &POSTGAMMA_BACKEND_STATE_VALUE(S_VALUE, Value);
    _Static_assert(__builtin_types_compatible_p(
        __typeof__(&POSTGAMMA_BACKEND_STATE_VALUE(S_VALUE, Value)),
        volatile int *), "volatile qualifier lost");
    return *pointer + *pointer;
}
void write_twice(void) {
    POSTGAMMA_BACKEND_STATE_VALUE(S_VALUE, Value) = 1;
    POSTGAMMA_BACKEND_STATE_VALUE(S_VALUE, Value) = 2;
}
'''
        if platform.machine() != "x86_64" or shutil.which("objdump") is None:
            self.skipTest("x86-64 disassembly tools are unavailable")
        with tempfile.TemporaryDirectory() as temporary:
            work = Path(temporary)
            (work / "volatile.c").write_text(source)
            for compiler in self.compilers():
                with self.subTest(compiler=shlex.join(compiler)):
                    obj = work / "volatile.o"
                    self.run_command([
                        *compiler, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                        "-I" + str(ROOT / "runtime/include"),
                        "-I" + str(ROOT / "tests/runtime/preparation_fixture/include"),
                        "-DPOSTGAMMA_PREPARED_ROLE_STATE", "-DPOSTGAMMA_DIRECT_ROLE_STATE",
                        "-c", str(work / "volatile.c"), "-o", str(obj),
                    ])
                    assembly = subprocess.check_output(["objdump", "-dr", str(obj)], text=True)
                    reads = re.search(r"<read_twice>:\n(.*?)(?=\n[0-9a-f]+ <|\Z)", assembly, re.S)[1]
                    writes = re.search(r"<write_twice>:\n(.*?)(?=\n[0-9a-f]+ <|\Z)", assembly, re.S)[1]
                    # Two distinct loads (mov/add) and both immediate stores
                    # must survive optimization, with no resolver table access.
                    self.assertEqual(len(re.findall(r"\b(?:mov|add)\s+0x[0-9a-f]+\(%r(?!ip)\w+\)", reads)), 2, reads)
                    self.assertRegex(writes, r"\$0x1,0x[0-9a-f]+\(")
                    self.assertRegex(writes, r"\$0x2,0x[0-9a-f]+\(")
                    self.assertNotIn("PostgammaCurrentBackendStateAddresses", assembly)

    def test_shared_frontend_source_discards_backend_slot_and_bridge(self):
        adapter = json.loads((ROOT / "manifests/postgresql/adapter.json").read_text())
        support = next(edit["content"] for edit in adapter["generated_support"]["edits"]
                       if "POSTGAMMA GENERATED BACKEND STATE SUPPORT" in edit.get("content", ""))
        source = support + """
static int value = 7;
int main(void) {
    POSTGAMMA_BACKEND_STATE_RELOCATED_SLOT_VALUE(undefined_slot++, value,
                                                undefined_bridge) += 1;
    return value != 8;
}
"""
        compilers = self.compilers()
        with tempfile.TemporaryDirectory() as temporary:
            work = Path(temporary)
            (work / "frontend.c").write_text(source)
            for compiler in compilers:
                for options in ([], ["-DPOSTGAMMA_BACKEND", "-DPOSTGAMMA_DIRECT_ROLE_STATE"]):
                    with self.subTest(compiler=shlex.join(compiler), options=options):
                        self.run_command([
                            *compiler, "-std=c11", "-O2", "-Wall", "-Wextra",
                            "-Werror", "-DFRONTEND", *options,
                            str(work / "frontend.c"), "-o", str(work / "frontend"),
                        ])
                        self.run_command([str(work / "frontend")])

    def test_constant_access_reuses_one_base_and_dynamic_access_stays_checked(self):
        if platform.machine() != "x86_64" or shutil.which("objdump") is None:
            self.skipTest("x86-64 disassembly tools are unavailable")
        source = """
#include "postgamma/backend_state_runtime.h"
static int Value;
static void *Zero;
int pair(void) {
    return POSTGAMMA_BACKEND_STATE_VALUE(S_VALUE, Value) +
           (POSTGAMMA_BACKEND_STATE_VALUE(S_ZERO, Zero) != NULL);
}
int dynamic(volatile PostgammaBackendStateSlot slot) {
    return POSTGAMMA_BACKEND_STATE_VALUE(slot, Value);
}
"""
        with tempfile.TemporaryDirectory() as temporary:
            work = Path(temporary)
            c = work / "access.c"
            c.write_text(source)
            for index, compiler in enumerate(self.compilers()):
                with self.subTest(compiler=shlex.join(compiler)):
                    obj = work / f"access-{index}.o"
                    command = [*compiler, "-std=c11", "-O2", "-DNDEBUG",
                               "-Wall", "-Wextra", "-Werror", "-c", str(c),
                               "-I" + str(ROOT / "runtime/include"),
                               "-I" + str(ROOT / "tests/runtime/preparation_fixture/include"),
                               "-DPOSTGAMMA_PREPARED_ROLE_STATE", "-DPOSTGAMMA_DIRECT_ROLE_STATE",
                               "-o", str(obj)]
                    self.run_command(command)
                    assembly = subprocess.check_output(["objdump", "-dr", str(obj)], text=True)
                    pair = re.search(r"<pair>:\n(.*?)(?=\n[0-9a-f]+ <|\Z)", assembly, re.S)[1]
                    dynamic = re.search(r"<dynamic>:\n(.*?)(?=\n[0-9a-f]+ <|\Z)", assembly, re.S)[1]
                    self.assertEqual(pair.count("PostgammaFixturePreparedRoleState"), 1, pair)
                    self.assertNotIn("PostgammaCurrentBackendStateAddresses", pair)
                    self.assertNotIn("postgamma_backend_state_address", pair)
                    self.assertIn("PostgammaCurrentBackendStateAddresses", dynamic)
                    self.assertIn("postgamma_backend_state_address", dynamic)
                    rejected = self.run_command(
                        [arg for arg in command if arg != "-DPOSTGAMMA_PREPARED_ROLE_STATE"],
                        check=False)
                    self.assertNotEqual(rejected.returncode, 0)
                    self.assertIn("requires prepared role state", rejected.stderr)

    def test_objects_from_different_preparation_sets_cannot_share_a_base(self):
        compiler = self.compilers()[0]
        fixture = registry.PreparationRegistryTests()
        fixture.setUp()
        self.addCleanup(fixture.doCleanups)
        report = fixture.report
        other = copy.deepcopy(report)
        next(slot for slot in other["slots"] if slot["preparation_eligible"])["offset"] += 8
        other["set_identity"] = preparation_set_identity(other)
        other["identity"] = digest({k: v for k, v in other.items() if k not in {"identity", "evidence"}})
        slots = ",".join(f"{s['enum_name']}={s['index']}" for s in report["slots"])
        comparisons = " || ".join(
            f"postgamma_backend_state_prepared_offset({s['enum_name']}) != " +
            (str(s["offset"]) if s["preparation_eligible"] else "SIZE_MAX")
            for s in report["slots"])
        preamble = "#include <stddef.h>\n#include <stdint.h>\n"
        preamble += f"typedef enum {{{slots}}} PostgammaBackendStateSlot;\n"
        with tempfile.TemporaryDirectory() as temporary:
            work = Path(temporary)
            (work / "getter.c").write_text(
                preamble + '#include "first.h"\n'
                'extern unsigned char *PostgammaCurrentPreparedRoleState;\n'
                'int main(void) { return PostgammaCurrentPreparedRoleState != 0 || ' +
                comparisons + '; }\n')
            (work / "binder.c").write_text(
                preamble + '#include "second.h"\n'
                'unsigned char *PostgammaCurrentPreparedRoleState;\n')
            (work / "first.h").write_text(emit_direct(report))
            command = [*compiler, "-O2", str(work / "getter.c"), str(work / "binder.c"),
                       "-o", str(work / "linked")]
            (work / "second.h").write_text(emit_direct(report))
            self.run_command(command)
            self.run_command([str(work / "linked")])
            (work / "second.h").write_text(emit_direct(other))
            failed = self.run_command(command, check=False)
            self.assertNotEqual(failed.returncode, 0)
            self.assertIn("PostgammaPreparedRoleState_" + report["set_identity"], failed.stderr)

            # The initializer and access header must also agree within one TU.
            include = work / "postgamma"
            include.mkdir()
            count = max(s["index"] for s in report["slots"]) + 1
            (include / "backend_state_layout.h").write_text(
                f"typedef enum {{{slots}, POSTGAMMA_BACKEND_STATE_SLOT_COUNT={count}}} "
                "PostgammaBackendStateSlot;\n")
            (include / "backend_state_direct.h").write_text(emit_direct(report))
            (work / "registry.inc").write_text(emit(report))
            source = work / "registry.c"
            source.write_text('#include "postgamma/backend_state_runtime.h"\n'
                              '#include "registry.inc"\n')
            command = [*compiler, "-std=c11", "-c", str(source),
                       "-I" + str(work), "-I" + str(ROOT / "runtime/include"),
                       "-DPOSTGAMMA_PREPARED_ROLE_STATE", "-DPOSTGAMMA_DIRECT_ROLE_STATE",
                       "-o", str(work / "registry.o")]
            self.run_command(command)
            (include / "backend_state_direct.h").write_text(emit_direct(other))
            failed = self.run_command(command, check=False)
            self.assertNotEqual(failed.returncode, 0)
            self.assertIn("direct access and preparation sets differ", failed.stderr)


class DirectStateCompilerTests(unittest.TestCase):
    def test_configured_compiler_and_arguments_reach_every_fixture(self):
        compiler = DirectStateAccessTests().compilers()[0]
        with tempfile.TemporaryDirectory() as temporary:
            work = Path(temporary)
            log = work / "commands.jsonl"
            launcher = work / "compiler launcher.py"
            launcher.write_text(
                "import json, os, sys\n"
                "with open(sys.argv[1], 'a') as log:\n"
                "    log.write(json.dumps(sys.argv[2:]) + '\\n')\n"
                "os.execvp(sys.argv[2], sys.argv[2:])\n",
                encoding="utf-8",
            )
            marker = "-DPOSTGAMMA_TEST_COMPILER_SELECTION=1"
            command = shlex.join(
                [sys.executable, str(launcher), str(log), *compiler, marker]
            )
            result = unittest.TestResult()
            with mock.patch.dict(os.environ, {"CC": command}):
                unittest.defaultTestLoader.loadTestsFromTestCase(
                    DirectStateAccessTests
                ).run(result)
            self.assertEqual(result.errors, [])
            self.assertEqual(result.failures, [])
            records = [json.loads(line) for line in log.read_text().splitlines()]
            sources = {
                Path(argument).name for arguments in records
                for argument in arguments if argument.endswith(".c")
            }
            expected = {"frontend.c", "getter.c", "binder.c", "registry.c"}
            if platform.machine() == "x86_64" and shutil.which("objdump"):
                expected.update(("access.c", "volatile.c"))
            self.assertEqual(sources, expected)
            for arguments in records:
                self.assertIn(marker, arguments)

    def test_configured_compiler_failure_is_reported_without_fallback(self):
        command = shlex.join([
            sys.executable, "-c",
            "import sys; print('compiler stdout'); "
            "print('compiler stderr', file=sys.stderr); sys.exit(9)",
        ])
        case = DirectStateAccessTests(
            "test_objects_from_different_preparation_sets_cannot_share_a_base"
        )
        result = unittest.TestResult()
        with mock.patch.dict(os.environ, {"CC": command}):
            case.run(result)
        self.assertEqual(result.errors, [])
        self.assertEqual(result.skipped, [])
        self.assertEqual(len(result.failures), 1)
        diagnostic = result.failures[0][1]
        self.assertIn("command failed (9)", diagnostic)
        self.assertIn(command, diagnostic)
        self.assertIn("stdout:\ncompiler stdout", diagnostic)
        self.assertIn("stderr:\ncompiler stderr", diagnostic)


if __name__ == "__main__":
    unittest.main()
