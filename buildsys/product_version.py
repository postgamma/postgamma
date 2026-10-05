#!/usr/bin/env python3
"""Validate the canonical PostGamma product version and release identity."""

from __future__ import annotations

import argparse
import json
import re
import subprocess
from dataclasses import dataclass
from pathlib import Path
from typing import Any

REPORT_KIND = "postgamma.product-version-evidence"
VERSION_PATTERN = re.compile(
    r"^(?P<major>0|[1-9][0-9]*)\."
    r"(?P<minor>0|[1-9][0-9]*)\."
    r"(?P<patch>0|[1-9][0-9]*)"
    r"(?:(?P<phase>a|b|rc)(?P<serial>[1-9][0-9]*))?$"
)


class ProductVersionError(RuntimeError):
    """The repository does not have one canonical release identity."""


@dataclass(frozen=True)
class ProductVersion:
    """A strict three-component public version with an optional prerelease."""

    text: str
    major: int
    minor: int
    patch: int
    phase: str | None
    serial: int | None

    @property
    def state(self) -> str:
        return {
            None: "stable",
            "a": "alpha",
            "b": "beta",
            "rc": "release-candidate",
        }[self.phase]

    @property
    def prerelease(self) -> bool:
        return self.phase is not None

    @property
    def tag(self) -> str:
        return f"v{self.text}"

    @property
    def ordering_key(self) -> tuple[int, int, int, int, int]:
        phase_rank = {"a": 0, "b": 1, "rc": 2, None: 3}[self.phase]
        return (
            self.major,
            self.minor,
            self.patch,
            phase_rank,
            self.serial or 0,
        )


def parse_version(value: str) -> ProductVersion:
    """Parse the supported PEP 440 subset used for public releases."""

    match = VERSION_PATTERN.fullmatch(value)
    if match is None:
        raise ProductVersionError(
            "version must be X.Y.Z, X.Y.ZaN, X.Y.ZbN, or X.Y.ZrcN"
        )
    phase = match.group("phase")
    serial = match.group("serial")
    return ProductVersion(
        text=value,
        major=int(match.group("major")),
        minor=int(match.group("minor")),
        patch=int(match.group("patch")),
        phase=phase,
        serial=int(serial) if serial is not None else None,
    )


def read_version(root: Path) -> ProductVersion:
    """Read the exact, newline-terminated root VERSION file."""

    path = root / "VERSION"
    content = path.read_text(encoding="ascii")
    if not content.endswith("\n") or content.count("\n") != 1:
        raise ProductVersionError("VERSION must contain one newline-terminated line")
    return parse_version(content[:-1])


def load_json(path: Path) -> dict[str, Any]:
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise ProductVersionError(f"cannot read {path}: {error}") from error
    if not isinstance(document, dict):
        raise ProductVersionError(f"{path} must contain a JSON object")
    return document


def previous_release_tag(
    root: Path, current: ProductVersion, tag: str | None
) -> tuple[bool, str | None]:
    """Require a tagged release to be newer than every earlier public tag."""

    if tag is None or not (root / ".git").exists():
        return False, None
    completed = subprocess.run(
        ["git", "tag", "--list", "v*"],
        cwd=root,
        check=False,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    if completed.returncode != 0:
        raise ProductVersionError(
            "cannot inspect existing release tags: " + completed.stderr.strip()
        )
    earlier: list[ProductVersion] = []
    for candidate_tag in completed.stdout.splitlines():
        if candidate_tag == tag:
            continue
        try:
            candidate = parse_version(candidate_tag.removeprefix("v"))
        except ProductVersionError:
            continue
        if candidate.ordering_key >= current.ordering_key:
            raise ProductVersionError(
                f"release {tag} is not newer than existing tag {candidate_tag}"
            )
        earlier.append(candidate)
    if not earlier:
        return True, None
    previous = max(earlier, key=lambda item: item.ordering_key)
    return True, previous.tag


def repository_identity(root: Path, tag: str | None = None) -> dict[str, Any]:
    """Require every checked release mirror to agree with VERSION."""

    version = read_version(root)
    if tag is not None and tag != version.tag:
        raise ProductVersionError(
            f"release tag {tag!r} does not match canonical tag {version.tag!r}"
        )
    monotonic_checked, previous_tag = previous_release_tag(root, version, tag)

    baseline_path = root / "manifests/api/python-api-v1.json"
    baseline = load_json(baseline_path)
    release = baseline.get("release")
    postgresql = baseline.get("postgresql")
    if not isinstance(release, dict) or not isinstance(postgresql, dict):
        raise ProductVersionError("Python release baseline has no release identity")
    postgresql_major = postgresql.get("major")
    if not isinstance(postgresql_major, int) or postgresql_major <= 0:
        raise ProductVersionError("Python release baseline has no PostgreSQL major")
    expected_build_id = f"postgamma-{version.text}-pg{postgresql_major}"
    if release.get("version") != version.text:
        raise ProductVersionError("Python release baseline differs from VERSION")
    if release.get("state") != version.state:
        raise ProductVersionError(
            "Python release state differs from the VERSION prerelease phase"
        )
    if postgresql.get("build_id") != expected_build_id:
        raise ProductVersionError("Python release build ID differs from VERSION")

    makefile = (root / "Makefile").read_text(encoding="utf-8")
    required_make_markers = (
        "POSTGAMMA_VERSION_FILE := $(ROOT)/VERSION",
        "override POSTGAMMA_PRODUCT_VERSION := $(strip $(file <$(POSTGAMMA_VERSION_FILE)))",
        "-DPOSTGAMMA_PRODUCT_VERSION='\"$(POSTGAMMA_PRODUCT_VERSION)\"'",
    )
    if any(marker not in makefile for marker in required_make_markers):
        raise ProductVersionError("Makefile is not bound to the canonical VERSION")
    if "POSTGAMMA_PRODUCT_VERSION ?=" in makefile:
        raise ProductVersionError("the product version must not be caller-overridable")

    backend = (root / "python/build_backend.py").read_text(encoding="utf-8")
    if 'VERSION_FILE = ROOT / "VERSION"' not in backend:
        raise ProductVersionError("Python build backend is not bound to VERSION")

    source = (root / "embedded-c/src/postgamma.c").read_text(encoding="utf-8")
    if (
        'return "postgamma-" POSTGAMMA_PRODUCT_VERSION "-pg"' not in source
        or "POSTGAMMA_POSTGRESQL_MAJOR;" not in source
    ):
        raise ProductVersionError("C build identity is not composed at build time")

    release_note = root / f"docs/releases/{version.text}.md"
    if not release_note.is_file():
        raise ProductVersionError(
            f"current release note is missing: {release_note.relative_to(root)}"
        )
    navigation = (root / "mkdocs.yml").read_text(encoding="utf-8")
    if f"- {version.text}: releases/{version.text}.md" not in navigation:
        raise ProductVersionError("documentation navigation omits the current version")

    return {
        "schema_version": 1,
        "kind": REPORT_KIND,
        "status": "pass",
        "version": version.text,
        "tag": version.tag,
        "state": version.state,
        "prerelease": version.prerelease,
        "postgresql_major": postgresql_major,
        "c_build_id": expected_build_id,
        "c_abi_version": postgresql.get("abi_version"),
        "canonical_source": "VERSION",
        "monotonic_tag_checked": monotonic_checked,
        "previous_release_tag": previous_tag,
        "checked_release_mirrors": [
            str(baseline_path.relative_to(root)),
            str(release_note.relative_to(root)),
            "Makefile",
            "embedded-c/src/postgamma.c",
            "python/build_backend.py",
        ],
    }


def write_github_output(path: Path, report: dict[str, Any]) -> None:
    values = {
        "version": report["version"],
        "tag": report["tag"],
        "state": report["state"],
        "prerelease": str(report["prerelease"]).lower(),
    }
    with path.open("a", encoding="utf-8") as stream:
        for name, value in values.items():
            stream.write(f"{name}={value}\n")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", required=True, type=Path)
    parser.add_argument("--tag")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--github-output", type=Path)
    args = parser.parse_args()
    try:
        report = repository_identity(args.root.resolve(), args.tag)
    except (OSError, ProductVersionError) as error:
        parser.error(str(error))
    if args.output is not None:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(
            json.dumps(report, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
    if args.github_output is not None:
        write_github_output(args.github_output, report)
    print(
        f"product version: {report['version']} ({report['state']}), "
        f"tag {report['tag']}, PostgreSQL {report['postgresql_major']}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
