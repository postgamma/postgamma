#!/usr/bin/env python3
"""Build, repair, and pip-check one release wheel using a prebuilt kernel."""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
from collections import deque
from pathlib import Path
from typing import Sequence

from check_python_wheel import WheelCheckError, load_release_baseline


ROOT = Path(__file__).resolve().parents[1]
OUTPUT_DIRECTORIES = ("reports", "raw", "wheel", "check")


class ReleaseRunError(RuntimeError):
    """A release stage failed; its output remains available for diagnosis."""


def prepare_output(output: Path, inputs: Sequence[Path]) -> None:
    """Clear only this runner's outputs, keeping inputs and unrelated files."""

    if output == ROOT or output in ROOT.parents:
        raise ReleaseRunError(f"output must be separate from the source root: {output}")
    for source in inputs:
        if source == output or source in output.parents or output in source.parents:
            raise ReleaseRunError(f"output overlaps a kernel input: {source}")
    directories = [output / name for name in OUTPUT_DIRECTORIES]
    for directory in directories:
        if directory.is_symlink() or (directory.exists() and not directory.is_dir()):
            raise ReleaseRunError(f"output must be a regular directory: {directory}")
    # Remove old pass evidence before starting any new stage, including retries.
    for directory in directories:
        if directory.exists():
            shutil.rmtree(directory)
        directory.mkdir(parents=True)


def run_stage(
    name: str, command: Sequence[str], environment: dict[str, str], reports: Path
) -> None:
    log = reports / f"{name}.log"
    print(f"[python-release] {name}: running; log: {log}", flush=True)
    with log.open("w", encoding="utf-8") as output:
        try:
            result = subprocess.run(
                list(command), cwd=ROOT, env=environment, stdin=subprocess.DEVNULL,
                stdout=output, stderr=subprocess.STDOUT, check=False,
            )
        except OSError as error:
            output.write(f"{error}\n")
            raise ReleaseRunError(f"{name} could not start: {error}; see {log}") from error
    if result.returncode:
        with log.open(encoding="utf-8", errors="replace") as output:
            # Preserve the checker's nested stdout/stderr diagnostic tails.
            tail = "".join(deque(output, maxlen=100))[-32768:].rstrip()
        raise ReleaseRunError(
            f"{name} failed with exit status {result.returncode}; see {log}\n{tail}"
        )
    print(f"[python-release] {name}: passed", flush=True)


def run_release(args: argparse.Namespace) -> Path:
    baseline_path = ROOT / "manifests/api/python-api-v1.json"
    baseline = load_release_baseline(baseline_path)
    platform = baseline["python"]["manylinux_policy"]
    if args.platform is not None and args.platform != platform:
        raise ReleaseRunError(f"platform must match the release baseline: {platform}")
    inputs = {
        "POSTGAMMA_STATIC_LIBRARY": args.static_library.resolve(),
        "POSTGAMMA_STATIC_LINK_OPTIONS": args.link_options.resolve(),
        "POSTGAMMA_STATIC_RECEIPT": args.static_receipt.resolve(),
        "POSTGAMMA_RESOURCE_ROOT": args.resource_root.resolve(),
    }
    output = args.output_dir.resolve()
    prepare_output(output, list(inputs.values()))
    for name, path in inputs.items():
        valid = path.is_dir() if name == "POSTGAMMA_RESOURCE_ROOT" else path.is_file()
        if not valid:
            raise ReleaseRunError(f"prebuilt kernel input is missing: {name}={path}")
    environment = dict(os.environ)
    environment.update({name: str(path) for name, path in inputs.items()})
    # The baseline owns repaired ZIP timestamps for both local and CI runs.
    environment["SOURCE_DATE_EPOCH"] = str(baseline["wheel"]["source_date_epoch"])
    reports, raw, wheel, check = (output / name for name in OUTPUT_DIRECTORIES)
    run_stage(
        "build",
        (sys.executable, str(ROOT / "python/build_backend.py"),
         "--wheel-dir", str(raw), "--receipt", str(reports / "raw-wheel.json")),
        environment, reports,
    )
    run_stage(
        "repair",
        (sys.executable, str(ROOT / "buildsys/repair_python_wheel.py"),
         "--wheel-dir", str(raw), "--receipt", str(reports / "raw-wheel.json"),
         "--output-dir", str(wheel),
         "--output-receipt", str(reports / "repaired-wheel.json"),
         "--output-report", str(reports / "manylinux-repair.json"),
         "--platform", platform, "--auditwheel", args.auditwheel),
        environment, reports,
    )
    evidence = reports / "python-wheel.json"
    run_stage(
        "check",
        (sys.executable, str(ROOT / "buildsys/check_python_wheel.py"),
         "--wheel-dir", str(wheel), "--receipt", str(reports / "repaired-wheel.json"),
         "--baseline", str(baseline_path), "--tests", str(ROOT / "tests/python"),
         "--integration", str(ROOT / "tests/python/integration_driver.py"),
         "--project-root", str(ROOT), "--work-root", str(check),
         "--forbidden-prefix", str(ROOT), "--installer", "pip",
         "--require-platform", platform, "--python", sys.executable,
         "--readelf", args.readelf, "--output", str(evidence)),
        environment, reports,
    )
    return evidence


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--static-library", required=True, type=Path)
    parser.add_argument("--link-options", required=True, type=Path)
    parser.add_argument("--static-receipt", required=True, type=Path)
    parser.add_argument("--resource-root", required=True, type=Path)
    parser.add_argument(
        "--output-dir", required=True, type=Path,
        help="recreate raw/, wheel/, reports/, and check/ below this directory",
    )
    parser.add_argument("--platform", help="must match the release baseline")
    parser.add_argument("--auditwheel", default="auditwheel")
    parser.add_argument("--readelf", default="readelf")
    args = parser.parse_args()
    try:
        evidence = run_release(args)
    except (OSError, WheelCheckError, ReleaseRunError) as error:
        parser.error(str(error))
    print(f"[python-release] passed: {evidence}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
