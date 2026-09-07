#!/usr/bin/env python3
"""Run GNU Make as a new top-level invocation for PostgreSQL test targets.

PostgreSQL intentionally creates its temporary installation only when
MAKELEVEL is zero.  Calling its `make check` through the superproject's
recursive `$(MAKE)` would therefore skip temp-install.
"""

from __future__ import annotations

import argparse
import os
import subprocess


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--make", default="make")
    parser.add_argument("--directory", required=True)
    parser.add_argument("arguments", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    if not args.arguments:
        parser.error("at least one Make target is required")

    environment = os.environ.copy()
    for name in ("MAKELEVEL", "MAKEFLAGS", "MFLAGS"):
        environment.pop(name, None)
    return subprocess.call(
        [args.make, "-C", args.directory, *args.arguments],
        env=environment,
    )


if __name__ == "__main__":
    raise SystemExit(main())
