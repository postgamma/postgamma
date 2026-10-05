"""Tests for reproducible PostgreSQL configure identities."""

from __future__ import annotations

import sys
import tempfile
import unittest
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(PROJECT_ROOT / "buildsys"))

import configure_postgres  # noqa: E402


class ConfigurePostgresTests(unittest.TestCase):
    def test_native_dependency_overrides_are_part_of_identity(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary)
            (source / "configure").write_text("#!/bin/sh\n", encoding="utf-8")
            environment = {
                "PATH": "/usr/bin",
                "ICU_CFLAGS": "-I/opt/icu/include",
                "ICU_LIBS": "-L/opt/icu/lib -licui18n -licuuc",
                "LZ4_CFLAGS": "-I/opt/lz4/include",
                "LZ4_LIBS": "-L/opt/lz4/lib -llz4",
                "PKG_CONFIG_SYSROOT_DIR": "/opt/sysroot",
            }
            request = configure_postgres.configuration_request(
                source, (), environment
            )
        self.assertEqual(request["environment"], environment)

    def test_implicit_compiler_search_paths_are_part_of_identity(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary)
            (source / "configure").write_text("#!/bin/sh\n", encoding="utf-8")
            request = configure_postgres.configuration_request(
                source,
                (),
                {
                    "CPATH": "/opt/include",
                    "LIBRARY_PATH": "/opt/lib",
                    "LD_LIBRARY_PATH": "/opt/runtime/lib",
                },
            )
        self.assertEqual(
            request["environment"],
            {
                "CPATH": "/opt/include",
                "LD_LIBRARY_PATH": "/opt/runtime/lib",
                "LIBRARY_PATH": "/opt/lib",
            },
        )


if __name__ == "__main__":
    unittest.main()
