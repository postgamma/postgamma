"""Tests for deterministic LLVM/Clang tool discovery."""

from __future__ import annotations

import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock


PROJECT_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(PROJECT_ROOT / "buildsys"))

import toolchain  # noqa: E402


class ToolchainTests(unittest.TestCase):
    def test_supported_versions_cover_validated_distribution_toolchains(self) -> None:
        self.assertEqual(toolchain.LLVM_MAJORS, (22, 21, 20, 19, 18, 17, 16))

    def test_versioned_llvm_precedes_a_moving_system_default(self) -> None:
        available = {
            "llvm-config-18": "/usr/bin/llvm-config-18",
            "llvm-config": "/usr/bin/llvm-config",
        }
        with tempfile.TemporaryDirectory() as temporary, mock.patch.dict(
            os.environ, {"LLVM_CONFIG": ""}
        ), mock.patch.object(
            toolchain,
            "_executable",
            side_effect=lambda value: available.get(str(value)),
        ):
            selected = toolchain.find_llvm_config(Path(temporary))
        self.assertEqual(selected, "/usr/bin/llvm-config-18")

    def test_versioned_clang_precedes_a_moving_system_default(self) -> None:
        available = {
            "clang++-17": "/usr/bin/clang++-17",
            "clang++": "/usr/bin/clang++",
        }
        with tempfile.TemporaryDirectory() as temporary, mock.patch.dict(
            os.environ, {"CLANGXX": ""}
        ), mock.patch.object(
            toolchain,
            "_executable",
            side_effect=lambda value: available.get(str(value)),
        ):
            selected = toolchain.find_clangxx(Path(temporary))
        self.assertEqual(selected, "/usr/bin/clang++-17")

    def test_versioned_clang_cpp_library_is_accepted(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            library = root / "libclang-cpp.so.20.1"
            library.touch()
            with mock.patch.object(
                toolchain,
                "_tool_query",
                side_effect=lambda _tool, argument: (
                    str(root) if argument == "--libdir" else "20.1.8"
                ),
            ):
                selected = toolchain.find_clang_cpp_library("llvm-config-20")
        self.assertEqual(selected, str(library))

    def test_clang_reports_its_distribution_specific_resource_dir(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            resource_dir = Path(temporary) / "lib" / "clang" / "22"
            resource_dir.mkdir(parents=True)
            with mock.patch.object(
                toolchain, "_tool_query", return_value=str(resource_dir)
            ):
                selected = toolchain.find_clang_resource_dir("clang++-22")
        self.assertEqual(selected, str(resource_dir))

    def test_missing_clang_resource_dir_is_rejected(self) -> None:
        with mock.patch.object(
            toolchain, "_tool_query", return_value="/missing/clang/resources"
        ):
            selected = toolchain.find_clang_resource_dir("clang++-22")
        self.assertIsNone(selected)


if __name__ == "__main__":
    unittest.main()
