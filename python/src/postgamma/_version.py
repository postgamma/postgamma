"""Resolve the installed distribution version or the source-tree version."""

from __future__ import annotations

from importlib.metadata import PackageNotFoundError, version
from pathlib import Path


def _source_version() -> str | None:
    source_root = Path(__file__).resolve().parents[3]
    source_module = source_root / "python/src/postgamma/_version.py"
    candidate = source_root / "VERSION"
    if not source_module.is_file():
        return None
    if not candidate.is_file():
        return None
    return candidate.read_text(encoding="ascii").strip()


__version__ = _source_version()
if __version__ is None:
    try:
        __version__ = version("postgamma")
    except PackageNotFoundError:
        __version__ = "0+unknown"
