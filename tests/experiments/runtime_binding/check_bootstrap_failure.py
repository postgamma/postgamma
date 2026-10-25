#!/usr/bin/env python3
"""Check pre-binding diagnostics against a server linked with bootstrap_failure.c."""

import argparse
import errno
import hashlib
import json
import os
from pathlib import Path
import resource
import subprocess


STAGES = {
    "instance-runtime": "could not create PostGamma instance runtime",
    "instance-context": "could not create PostGamma instance context",
    "role-context": "could not create PostGamma role context",
    "execution-context": "could not create PostGamma execution context",
    "role-preparation": "could not prepare PostGamma role state",
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--prefix", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    prefix, output = args.prefix.resolve(strict=True), args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    executable = prefix / "bin/postgres"
    environment = dict(os.environ, LD_LIBRARY_PATH=str(prefix / "lib"), LC_ALL="C")
    environment.pop("LD_PRELOAD", None)
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    report = {"status": "running", "stages": [],
              "binary_sha256": hashlib.sha256(executable.read_bytes()).hexdigest()}
    try:
        for stage, diagnostic in STAGES.items():
            # --version cannot start a database even if a wrapper is missing.
            # Threaded-server bootstrap runs before option processing.
            result = subprocess.run(
                [str(executable), "--version"],
                env=dict(environment, POSTGAMMA_TEST_BOOTSTRAP_FAILURE=stage),
                capture_output=True, text=True, timeout=30)
            (output / (stage + ".stdout")).write_text(result.stdout)
            (output / (stage + ".stderr")).write_text(result.stderr)
            if (result.returncode != 1 or
                    f"POSTGAMMA_TEST_BOOTSTRAP_FAILURE {stage}\n" not in result.stderr or
                    f"{diagnostic}: error {errno.ENOMEM}\n" not in result.stderr):
                raise RuntimeError(f"{stage}: expected diagnosed failure, got "
                                   f"exit {result.returncode}: {result.stderr}")
            report["stages"].append(stage)
        report["status"] = "passed"
    except BaseException as error:
        report.update(status="failed", error=repr(error))
        raise
    finally:
        (output / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    print(f"server bootstrap: {len(report['stages'])} pre-binding failures passed")


if __name__ == "__main__":
    main()
