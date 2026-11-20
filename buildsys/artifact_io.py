"""Atomic publication of build evidence and generated text."""

from __future__ import annotations

import hashlib
import json
import os
import tempfile
from pathlib import Path


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def write_if_changed(path: Path, content: str) -> None:
    """Preserve unchanged generated output, including its timestamp."""
    data = content.encode("utf-8")
    if path.is_file() and path.read_bytes() == data:
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_bytes(data)
    temporary.replace(path)


def _replace_text(path: Path, content: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    handle = tempfile.NamedTemporaryFile(
        mode="w", encoding="utf-8", dir=path.parent, prefix=f".{path.name}.", delete=False
    )
    temporary = Path(handle.name)
    try:
        with handle:
            handle.write(content)
        os.replace(temporary, path)
    finally:
        temporary.unlink(missing_ok=True)


def write_json(path: Path, document: object) -> None:
    _replace_text(path, json.dumps(document, indent=2, sort_keys=True) + "\n")


def write_generated_text(path: Path, content: str) -> None:
    """Keep unchanged output's inode, but advance its timestamp for make."""
    if path.exists() and path.read_text(encoding="utf-8") == content:
        os.utime(path, None)
        return
    _replace_text(path, content)
