"""Bounded native tests for session-owned platform adapters, without a PG build."""

from __future__ import annotations

import json
import os
import shlex
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


class RuntimeAdapterTests(unittest.TestCase):
    def run_make(self, name: str, *assignments: str) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            command = [
                *shlex.split(os.environ.get("MAKE", "make")),
                "--no-print-directory", "-s", f"runtime-{name}-test",
                f"BUILD_DIR={temporary}", f"CC={os.environ.get('CC', 'cc')}",
                *assignments,
            ]
            result = subprocess.run(
                command,
                cwd=ROOT,
                capture_output=True,
                text=True,
                timeout=90,
            )
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_session_locale_survives_carrier_switches_and_failed_updates(self) -> None:
        self.run_make("session-locale")

    def test_external_processes_reap_owned_children_after_nonlocal_exit(self) -> None:
        self.run_make("external-process")

    def test_virtual_signals_and_timers_keep_session_state_off_the_host(self) -> None:
        self.run_make("virtual-signal")

    def test_adapter_checks_execute_with_ndebug(self) -> None:
        for name in ("session-locale", "external-process", "virtual-signal"):
            with self.subTest(adapter=name):
                self.run_make(name, "CFLAGS=-DNDEBUG")

    def test_failed_check_remains_fatal_with_ndebug(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary) / "check.c"
            binary = Path(temporary) / "check"
            source.write_text(
                '#include "check.h"\n'
                'int main(void) { int calls = 0; CHECK(++calls == 1); '
                'CHECK(calls == 2); return 0; }\n'
            )
            subprocess.run(
                [*shlex.split(os.environ.get("CC", "cc")), "-std=c11", "-DNDEBUG",
                 "-Wall", "-Wextra", "-Werror", f"-I{ROOT / 'tests'}",
                 str(source), "-o", str(binary)],
                check=True, capture_output=True, text=True, timeout=30,
            )
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 1)
            self.assertIn("check failed: calls == 2", result.stderr)

    def test_adapters_are_dependencies_of_both_runtime_gates(self) -> None:
        result = subprocess.run(
            [*shlex.split(os.environ.get("MAKE", "make")), "-qp", "runtime-adapter-test"],
            cwd=ROOT, capture_output=True, text=True, timeout=30,
        )
        self.assertIn(result.returncode, (0, 1), result.stderr)
        targets = {}
        for line in result.stdout.splitlines():
            if line.startswith("runtime-") and ":" in line:
                name, dependencies = line.split(":", 1)
                targets[name] = set(dependencies.split())
        self.assertIn("runtime-adapter-test", targets["runtime-test"])
        self.assertIn("runtime-adapter-sanitizer-test", targets["runtime-sanitizer-test"])
        for name in ("session-locale", "external-process", "virtual-signal"):
            self.assertIn(f"runtime-{name}-test", targets["runtime-adapter-test"])
            self.assertIn(f"runtime-{name}-sanitizer-test",
                          targets["runtime-adapter-sanitizer-test"])

    def test_platform_adapters_are_installed_and_linked_in_generated_trees(
        self,
    ) -> None:
        adapter = json.loads((ROOT / "manifests/postgresql/adapter.json").read_text())
        support = adapter["generated_support"]
        copies = {entry["source"]: entry["target"] for entry in support["copies"]}
        # Check source and header installation together with the PostgreSQL object rule.
        content = json.dumps(support)
        for name in ("session_locale", "external_process", "virtual_signal"):
            source = "postgres_session_locale" if name == "session_locale" else name
            self.assertEqual(
                copies[f"runtime/src/{source}.c"],
                f"src/backend/utils/init/postgamma_{name}.c",
            )
            self.assertEqual(
                copies[f"runtime/include/postgamma/{name}.h"],
                f"src/include/postgamma/{name}.h",
            )
            self.assertIn(f"postgamma_{name}.o", content)
        self.assertEqual(copies["runtime/src/session_locale.c"],
                         "src/backend/utils/init/postgamma_session_locale_impl.inc")

    def test_generated_locale_wrapper_respects_postgresql_configuration(self) -> None:
        # Compile the actual installed wrapper with enabled/disabled pg_config.h.
        # A poisoned locale.h proves the disabled build never includes locale APIs.
        adapter = json.loads((ROOT / "manifests/postgresql/adapter.json").read_text())
        with tempfile.TemporaryDirectory() as temporary:
            tree = Path(temporary)
            for entry in adapter["generated_support"]["copies"]:
                if entry["id"].startswith("session-locale-"):
                    target = tree / entry["target"]
                    target.parent.mkdir(parents=True, exist_ok=True)
                    shutil.copyfile(ROOT / entry["source"], target)
            include = tree / "src/include"
            wrapper = tree / "src/backend/utils/init/postgamma_session_locale.c"
            compiler = shlex.split(os.environ.get("CC", "cc"))
            flags = ["-std=c11", "-Wall", "-Wextra", "-Werror", "-Wpedantic",
                     f"-I{include}", "-Iruntime/include", "-Iembedded-c/include"]
            for config in ("/* HAVE_USELOCALE unavailable */\n",
                           "#define HAVE_USELOCALE 1\n#define WIN32 1\n"):
                (include / "pg_config.h").write_text(config)
                (include / "locale.h").write_text('#error locale APIs must not be included\n')
                subprocess.run([*compiler, *flags, "-c", str(wrapper),
                                "-o", str(tree / "disabled.o")],
                               cwd=ROOT, check=True, capture_output=True, text=True, timeout=30)
            (include / "locale.h").unlink()
            (include / "pg_config.h").write_text("#define HAVE_USELOCALE 1\n")
            binary = tree / "enabled"
            subprocess.run([*compiler, *flags, str(wrapper),
                            "tests/runtime/test_session_locale.c", "-pthread", "-o", str(binary)],
                           cwd=ROOT, check=True, capture_output=True, text=True, timeout=30)
            subprocess.run([str(binary)], check=True, capture_output=True, text=True, timeout=30)
