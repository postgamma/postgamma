#!/usr/bin/env python3
"""Capture the actual Clang invocation supplied by the Emscripten driver."""

import os
import sys

from cc_capture import record


def main() -> None:
    # EM_COMPILER_WRAPPER receives the compiler followed by its expanded flags.
    # Recording here preserves the target/sysroot and excludes emcc-only -s flags.
    compiler, *arguments = sys.argv[1:]
    record(compiler, arguments)
    os.execv(compiler, [compiler, *arguments])


if __name__ == "__main__":
    main()
