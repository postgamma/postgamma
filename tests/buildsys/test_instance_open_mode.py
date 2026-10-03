"""Compile real public open entrypoints; substitute only kernel/creation services."""

from __future__ import annotations

import json
import os
import shlex
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]


class InstanceOpenModeTests(unittest.TestCase):
    def run_command(self, command: list[str]) -> subprocess.CompletedProcess[str]:
        result = subprocess.run(
            command,
            cwd=ROOT,
            capture_output=True,
            text=True,
            timeout=60,
        )
        self.assertEqual(
            result.returncode,
            0,
            f"command failed ({result.returncode}): {shlex.join(command)}\n"
            f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}",
        )
        return result

    def test_native_checks_use_configured_tool_commands(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            work = Path(temporary)
            log = work / "commands.jsonl"
            launcher = work / "tool launcher.py"
            launcher.write_text(
                "import json, os, sys\n"
                "with open(sys.argv[1], 'a') as log:\n"
                "    log.write(json.dumps({'tool': sys.argv[2], "
                "'argv': sys.argv[3:]}) + '\\n')\n"
                "os.execvp(sys.argv[3], sys.argv[3:])\n",
                encoding="utf-8",
            )
            environment = {}
            for variable, fallback in (("CC", "cc"), ("CXX", "c++"), ("AR", "ar")):
                command = shlex.split(os.environ.get(variable, fallback))
                if variable != "AR":
                    command.append("-DPOSTGAMMA_TEST_TOOL_SELECTION=1")
                environment[variable] = shlex.join(
                    [sys.executable, str(launcher), str(log), variable, *command]
                )
            with mock.patch.dict(os.environ, environment):
                self.check_open_policy()
                self.test_c_and_cpp_layouts_and_numerics_match_the_frozen_contract()
            records = [json.loads(line) for line in log.read_text().splitlines()]
            self.assertEqual(
                {record["tool"] for record in records}, {"CC", "CXX", "AR"}
            )
            consumers = [
                record for record in records
                if "tests/embedded/api/api_contract_consumer.c" in record["argv"]
            ]
            self.assertEqual([record["tool"] for record in consumers], ["CC", "CXX"])
            for record, language in zip(consumers, ("c", "c++")):
                arguments = record["argv"]
                self.assertIn("-DPOSTGAMMA_TEST_TOOL_SELECTION=1", arguments)
                self.assertIn("-x", arguments)
                self.assertEqual(arguments[arguments.index("-x") + 1], language)

    def test_abi_compiler_failure_includes_command_and_output(self) -> None:
        compiler = shlex.join(
            [
                sys.executable,
                "-c",
                "import sys; print('compiler stdout'); "
                "print('compiler stderr', file=sys.stderr); sys.exit(9)",
            ]
        )
        case = InstanceOpenModeTests(
            "test_c_and_cpp_layouts_and_numerics_match_the_frozen_contract"
        )
        result = unittest.TestResult()
        with mock.patch.dict(os.environ, {"CXX": compiler}):
            case.run(result)
        self.assertEqual(result.errors, [])
        self.assertEqual(len(result.failures), 1)
        diagnostic = result.failures[0][1]
        self.assertIn("command failed (9)", diagnostic)
        self.assertIn(compiler, diagnostic)
        self.assertIn("stdout:\ncompiler stdout", diagnostic)
        self.assertIn("stderr:\ncompiler stderr", diagnostic)

    def test_open_policy_and_independently_compiled_abi_13_caller(self) -> None:
        self.check_open_policy()

    def test_open_policy_checks_execute_with_ndebug(self) -> None:
        self.check_open_policy(("-DNDEBUG",))

    def check_open_policy(self, extra_flags: tuple[str, ...] = ()) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            work = Path(temporary)
            cc = [*shlex.split(os.environ.get("CC", "cc")), *extra_flags]
            flags = [
                "-std=c11",
                "-D_POSIX_C_SOURCE=200809L",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-ffunction-sections",
                "-fdata-sections",
                "-Iembedded-c/include",
                "-Iruntime/include",
                '-DPOSTGAMMA_PRODUCT_VERSION="test"',
                '-DPOSTGAMMA_POSTGRESQL_MAJOR="19"',
            ]

            # The legacy object sees only frozen 1.3 declarations, never the new header.
            old = work / "legacy.o"
            self.run_command(
                [
                    *cc,
                    "-std=c11",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-c",
                    "tests/embedded/api/instance_open_legacy.c",
                    "-o",
                    str(old),
                ]
            )
            obj = work / "postgamma.o"
            self.run_command(
                [*cc, *flags, "-c", "embedded-c/src/postgamma.c", "-o", str(obj)]
            )
            archive = work / "libopen.a"
            self.run_command(
                [*shlex.split(os.environ.get("AR", "ar")),
                 "crs", str(archive), str(obj)]
            )
            binary = work / "open-mode"
            self.run_command(
                [
                    *cc,
                    *flags,
                    "tests/embedded/api/instance_open_mode.c",
                    str(old),
                    str(archive),
                    "embedded-c/src/supervisor.c",
                    "embedded-c/src/data_directory_lock.c",
                    "embedded-c/src/kernel_supervisor_adapter.c",
                    "runtime/src/thread_runtime.c",
                    "-pthread",
                    "-Wl,--gc-sections",
                    "-o",
                    str(binary),
                ]
            )
            self.assertIn(
                "old caller boundary: pass",
                self.run_command([str(binary), str(work)]).stdout,
            )

    def test_c_and_cpp_layouts_and_numerics_match_the_frozen_contract(self) -> None:
        sys.path.insert(0, str(ROOT / "buildsys"))
        from check_c_api_contract import parse_consumer_output, flatten_numeric_registry

        policy = json.loads((ROOT / "manifests/api/c-public-api.json").read_text())
        frozen = json.loads((ROOT / "manifests/api/c-core-abi-v1.json").read_text())
        with tempfile.TemporaryDirectory() as temporary:
            for variable, fallback, language, standard in (
                ("CC", "cc", "c", "c11"),
                ("CXX", "c++", "c++", "c++17"),
            ):
                compiler = shlex.split(os.environ.get(variable, fallback))
                with self.subTest(compiler=compiler):
                    binary = Path(temporary) / variable.lower()
                    self.run_command(
                        [
                            *compiler,
                            "-x",
                            language,
                            f"-std={standard}",
                            "-Wall",
                            "-Wextra",
                            "-Werror",
                            "-Iembedded-c/include",
                            "tests/embedded/api/api_contract_consumer.c",
                            "-o",
                            str(binary),
                        ]
                    )
                    output = self.run_command([str(binary)]).stdout
                    facts = parse_consumer_output(output)
                    self.assertEqual(
                        facts["numerics"], flatten_numeric_registry(policy)
                    )
                    for record in frozen["records"]:
                        layout = facts["layouts"][record["name"]]
                        self.assertEqual(layout["size"], record["minimum_size"])
                        self.assertEqual(layout["alignment"], record["alignment"])
                        self.assertEqual(
                            layout["fields"],
                            {f["name"]: f["offset"] for f in record["fields"]},
                        )
