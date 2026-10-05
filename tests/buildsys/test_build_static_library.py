"""Tests for the final static-library closure builder."""

from __future__ import annotations

import sys
import tempfile
import unittest
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(PROJECT_ROOT / "buildsys"))

import build_static_library  # noqa: E402


class StaticLibraryTests(unittest.TestCase):
    def test_response_partition_keeps_inputs_and_external_libraries(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "kernel.o").write_bytes(b"object")
            (root / "support.a").write_bytes(b"archive")
            partial, consumer, inputs = build_static_library.classify_response(
                [
                    "-Wall",
                    "kernel.o",
                    "support.a",
                    "-L../private",
                    "-Wl,--as-needed",
                    "-lz",
                    "-lm",
                    "-pthread",
                ],
                root,
            )
            self.assertEqual(partial, [str(root / "kernel.o"), str(root / "support.a")])
            self.assertEqual(consumer, ["-lz", "-lm", "-pthread"])
            self.assertEqual(inputs, [root / "kernel.o", root / "support.a"])

    def test_response_rejects_a_missing_closure_input(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            with self.assertRaisesRegex(
                build_static_library.StaticLibraryError, "does not exist"
            ):
                build_static_library.classify_response(
                    ["missing.o", "-lm"], Path(temporary)
                )

    def test_pkg_config_is_relocatable_and_exposes_private_dependencies(self) -> None:
        content = build_static_library.pkg_config_content(
            "1.3", ["-pthread", "-lz", "-lm"]
        )
        self.assertIn("prefix=${pcfiledir}/../..", content)
        self.assertIn("URL: https://postgamma.com", content)
        self.assertIn("Libs: ${libdir}/libpostgamma.a", content)
        self.assertIn("Libs.private: -pthread -lz -lm", content)
        self.assertNotIn(str(PROJECT_ROOT), content)


if __name__ == "__main__":
    unittest.main()
