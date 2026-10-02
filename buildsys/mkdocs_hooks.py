"""MkDocs hooks for inserting fail-closed generated API references."""

from __future__ import annotations

import json
import os
from pathlib import Path
from typing import Any


GENERATED_REFERENCES = {
    "<!-- POSTGAMMA_GENERATED_C_API -->": "c-api.md",
    "<!-- POSTGAMMA_GENERATED_EXTENSION_API -->": "extension-api.md",
    "<!-- POSTGAMMA_GENERATED_PYTHON_API -->": "python-api.md",
}
VERSION_MARKER = "{{ POSTGAMMA_VERSION }}"
RELEASES_URL_MARKER = "{{ POSTGAMMA_RELEASES_URL }}"
RELEASE_STATE_MARKER = "{{ POSTGAMMA_RELEASE_STATE }}"


def on_page_markdown(
    markdown: str, page: Any, config: Any, files: Any
) -> str:
    """Replace one reference marker with its build-generated Markdown."""

    del page, files
    root = Path(config.config_file_path).resolve().parent
    version = (root / "VERSION").read_text(encoding="ascii").strip()
    baseline = json.loads(
        (root / "manifests/api/python-api-v1.json").read_text(encoding="utf-8")
    )
    release_state = baseline["release"]["state"]
    releases_url = os.environ.get("POSTGAMMA_RELEASES_URL", "#release-status")
    markdown = markdown.replace(VERSION_MARKER, version)
    markdown = markdown.replace(RELEASE_STATE_MARKER, release_state)
    markdown = markdown.replace(RELEASES_URL_MARKER, releases_url)
    configured = os.environ.get("POSTGAMMA_DOCS_GENERATED_DIR")
    generated = (
        Path(configured).resolve()
        if configured
        else root / "build" / "docs" / "generated"
    )
    for marker, filename in GENERATED_REFERENCES.items():
        count = markdown.count(marker)
        if count == 0:
            continue
        if count != 1:
            raise RuntimeError(f"documentation marker occurs {count} times: {marker}")
        path = generated / filename
        if not path.is_file():
            raise RuntimeError(
                f"generated API reference is missing: {path}; run make docs-api"
            )
        markdown = markdown.replace(marker, path.read_text(encoding="utf-8"))
    return markdown
