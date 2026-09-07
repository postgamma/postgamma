#!/usr/bin/env python3
"""Print one discovered build tool for Makefile consumption."""

from __future__ import annotations

import argparse
from pathlib import Path

from toolchain import (
    find_clang_cpp_library,
    find_clang_resource_dir,
    find_clangxx,
    find_llvm_config,
)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "tool",
        choices=(
            "llvm-config",
            "clang++",
            "clang-cpp-library",
            "clang-resource-dir",
        ),
    )
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    args = parser.parse_args()
    root = args.root.resolve()
    llvm_config = find_llvm_config(root)
    clangxx = find_clangxx(root, llvm_config)
    if args.tool == "llvm-config":
        value = llvm_config
    elif args.tool == "clang++":
        value = clangxx
    elif args.tool == "clang-cpp-library":
        value = find_clang_cpp_library(llvm_config)
    else:
        value = find_clang_resource_dir(clangxx)
    if value:
        print(value)
        return 0
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
