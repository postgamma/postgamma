"""Compile real public open entrypoints; substitute only kernel/creation services."""

from __future__ import annotations

import json
import os
import shlex
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


class InstanceOpenModeTests(unittest.TestCase):
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

            def run(command):
                return subprocess.run(
                    command,
                    cwd=ROOT,
                    check=True,
                    capture_output=True,
                    text=True,
                    timeout=60,
                )

            # The legacy object sees only frozen 1.3 declarations, never the new header.
            old = work / "legacy.o"
            run(
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
            run([*cc, *flags, "-c", "embedded-c/src/postgamma.c", "-o", str(obj)])
            archive = work / "libopen.a"
            run(["ar", "crs", str(archive), str(obj)])
            binary = work / "open-mode"
            run(
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
                "old caller boundary: pass", run([str(binary), str(work)]).stdout
            )

    def test_c_and_cpp_layouts_and_numerics_match_the_frozen_contract(self) -> None:
        import sys

        sys.path.insert(0, str(ROOT / "buildsys"))
        from check_c_api_contract import parse_consumer_output, flatten_numeric_registry

        policy = json.loads((ROOT / "manifests/api/c-public-api.json").read_text())
        frozen = json.loads((ROOT / "manifests/api/c-core-abi-v1.json").read_text())
        with tempfile.TemporaryDirectory() as temporary:
            for compiler, standard in (("cc", "c11"), ("c++", "c++17")):
                with self.subTest(compiler=compiler):
                    binary = Path(temporary) / compiler.replace("+", "p")
                    subprocess.run(
                        [
                            compiler,
                            f"-std={standard}",
                            "-Wall",
                            "-Wextra",
                            "-Werror",
                            "-Iembedded-c/include",
                            "tests/embedded/api/api_contract_consumer.c",
                            "-o",
                            str(binary),
                        ],
                        cwd=ROOT,
                        check=True,
                        capture_output=True,
                    )
                    output = subprocess.check_output([str(binary)], text=True)
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
