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


def on_files(files: Any, config: Any) -> Any:
    """Copy a verified browser package without checking generated binaries in."""
    from mkdocs.structure.files import File

    root = Path(config.config_file_path).resolve().parent
    directory = Path(os.environ.get("POSTGAMMA_DOCS_WASM_DIR", root / "build/docs/browser"))
    if not (directory / "assets/wasm/demo.json").is_file():
        raise RuntimeError("browser demo metadata is missing; run make docs-wasm")
    for path in sorted(directory.rglob("*")):
        if path.is_file() and path.suffix not in {".md", ".ts"}:
            files.append(File(path.relative_to(directory).as_posix(), str(directory), config.site_dir, False))
    return files


def on_serve(server: Any, **kwargs: Any) -> Any:
    """Local preview uses native isolation headers, retaining MkDocs live reload."""
    app = server.get_app()

    def isolated(environ: Any, start_response: Any) -> Any:
        def respond(status: str, headers: list, exc_info: Any = None) -> Any:
            headers = [(k, v) for k, v in headers if k.lower() not in {
                "cross-origin-opener-policy", "cross-origin-embedder-policy"}]
            headers.extend([("Cross-Origin-Opener-Policy", "same-origin"),
                            ("Cross-Origin-Embedder-Policy", "require-corp")])
            return start_response(status, headers, exc_info)
        return app(environ, respond)

    server.set_app(isolated)
    return server


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
