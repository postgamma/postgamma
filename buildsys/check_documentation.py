#!/usr/bin/env python3
"""Validate public documentation coverage before building the static site."""

from __future__ import annotations

import argparse
import ast
import json
import re
from pathlib import Path
from typing import Any

from generate_python_api_docs import (
    DocumentationError as PythonDocumentationError,
)
from generate_python_api_docs import load_json, validate


C_DECLARATION = re.compile(
    r"\bPGM_API\b[^;]*?\b(pgm_[a-zA-Z0-9_]+)\s*\(", re.DOTALL
)
EXTENSION_DECLARATION = re.compile(
    r"(?:^|\n)(?:int\s+|uint64_t\s+|void\s*\*\s*)"
    r"(pgmex_[a-zA-Z0-9_]+)\s*\(",
    re.MULTILINE,
)
REFERENCE_MARKERS = {
    "<!-- POSTGAMMA_GENERATED_C_API -->": "docs/c/reference.md",
    "<!-- POSTGAMMA_GENERATED_EXTENSION_API -->": "docs/extensions/reference.md",
    "<!-- POSTGAMMA_GENERATED_PYTHON_API -->": "docs/python/reference.md",
}
FENCE = re.compile(r"^ {0,3}(`{3,}|~{3,})")
REQUIRED_DOCUMENTS = (
    "docs/index.md",
    "docs/getting-started/index.md",
    "docs/getting-started/why-postgamma.md",
    "docs/getting-started/c-static.md",
    "docs/getting-started/installation.md",
    "docs/getting-started/database-paths.md",
    "docs/downloads.md",
    "docs/concepts/architecture.md",
    "docs/concepts/concurrency.md",
    "docs/concepts/transactions.md",
    "docs/concepts/durability.md",
    "docs/concepts/resource-footprint.md",
    "docs/concepts/security.md",
    "docs/python/index.md",
    "docs/python/database-and-dbapi.md",
    "docs/python/asyncio.md",
    "docs/python/streaming-and-copy.md",
    "docs/python/management.md",
    "docs/python/integration.md",
    "docs/python/types.md",
    "docs/python/reference.md",
    "docs/c/index.md",
    "docs/c/build-and-link.md",
    "docs/c/ownership-and-concurrency.md",
    "docs/c/async.md",
    "docs/c/streaming-and-copy.md",
    "docs/c/management.md",
    "docs/c/reference.md",
    "docs/extensions/index.md",
    "docs/extensions/pgvector.md",
    "docs/extensions/reference.md",
    "docs/reference/errors.md",
    "docs/compatibility/platforms.md",
    "docs/compatibility/postgresql.md",
    "docs/compatibility/limits.md",
    "docs/troubleshooting.md",
    "docs/releases/index.md",
    "docs/releases/versioning.md",
    "docs/releases/0.1.0a1.md",
    "docs/about/license.md",
)
PYTHON_EXAMPLE_MANIFEST = "docs/reference/python-examples.json"
STRUCTURED_PYTHON_DOCSTRINGS = {
    "connect": ("Args:", "Returns:", "Raises:"),
    "connect_async": ("Args:", "Returns:", "Raises:"),
    "Database": ("Args:",),
    "Database.open": ("Returns:", "Raises:"),
    "Database.connect": ("Args:", "Returns:", "Raises:"),
    "Connection.execute": ("Args:", "Returns:", "Raises:"),
}


class DocumentationCheckError(RuntimeError):
    """Raised when public documentation is incomplete or stale."""


def read_json(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise DocumentationCheckError(f"cannot read {path}: {error}") from error
    if not isinstance(value, dict):
        raise DocumentationCheckError(f"{path} must contain a JSON object")
    return value


def exact_string_map(value: object, label: str) -> dict[str, str]:
    if not isinstance(value, dict) or not all(
        isinstance(name, str) and isinstance(description, str) and description.strip()
        for name, description in value.items()
    ):
        raise DocumentationCheckError(f"{label} must be a nonempty string map")
    return {name: description.strip() for name, description in value.items()}


def check_c_api(root: Path) -> tuple[int, int]:
    policy = read_json(root / "manifests/api/c-public-api.json")
    docs = read_json(root / "docs/reference/c-api.json")
    entries = policy.get("functions")
    if not isinstance(entries, list):
        raise DocumentationCheckError("C API function policy must be a list")
    names = [
        entry.get("name") for entry in entries if isinstance(entry, dict)
    ]
    if len(names) != len(entries) or not all(isinstance(name, str) for name in names):
        raise DocumentationCheckError("C API function policy contains an invalid entry")
    expected = set(names)
    if len(expected) != len(names):
        raise DocumentationCheckError("C API function policy contains duplicates")

    header_text = "\n".join(
        (root / path).read_text(encoding="utf-8")
        for path in policy.get("headers", [])
    )
    declared = set(C_DECLARATION.findall(header_text))
    summaries = exact_string_map(docs.get("functions"), "C API summaries")
    if declared != expected or set(summaries) != expected:
        raise DocumentationCheckError(
            "C API documentation is not exact: "
            f"header_missing={sorted(expected - declared)}, "
            f"header_extra={sorted(declared - expected)}, "
            f"docs_missing={sorted(expected - set(summaries))}, "
            f"docs_stale={sorted(set(summaries) - expected)}"
        )

    extension_header = (
        root / "embedded-c/include/postgamma/postgamma_extension.h"
    ).read_text(encoding="utf-8")
    declared_extension = set(EXTENSION_DECLARATION.findall(extension_header))
    extension_summaries = exact_string_map(
        docs.get("extension_functions"), "extension API summaries"
    )
    if declared_extension != set(extension_summaries):
        raise DocumentationCheckError(
            "extension API documentation is not exact: "
            f"missing={sorted(declared_extension - set(extension_summaries))}, "
            f"stale={sorted(set(extension_summaries) - declared_extension)}"
        )
    return len(expected), len(declared_extension)


def public_all(module: ast.Module) -> list[str]:
    for statement in module.body:
        if not isinstance(statement, ast.Assign):
            continue
        if not any(
            isinstance(target, ast.Name) and target.id == "__all__"
            for target in statement.targets
        ):
            continue
        try:
            value = ast.literal_eval(statement.value)
        except (ValueError, TypeError) as error:
            raise DocumentationCheckError("postgamma.__all__ is not literal") from error
        if not isinstance(value, list) or not all(isinstance(item, str) for item in value):
            raise DocumentationCheckError("postgamma.__all__ must be a string list")
        return value
    raise DocumentationCheckError("postgamma.__all__ was not found")


def object_origins(module: ast.Module) -> dict[str, tuple[str, str]]:
    origins: dict[str, tuple[str, str]] = {}
    for statement in module.body:
        if isinstance(statement, ast.ImportFrom) and statement.module:
            for alias in statement.names:
                origins[alias.asname or alias.name] = (statement.module, alias.name)
        elif isinstance(statement, (ast.ClassDef, ast.FunctionDef, ast.AsyncFunctionDef)):
            origins[statement.name] = ("__init__", statement.name)
    return origins


def source_object(root: Path, module_name: str, object_name: str) -> ast.AST | None:
    package = root / "python/src/postgamma"
    path = (
        package / "__init__.py"
        if module_name == "__init__"
        else package / (module_name.lstrip(".") + ".py")
    )
    tree = ast.parse(path.read_text(encoding="utf-8"), filename=str(path))
    return next(
        (
            statement
            for statement in tree.body
            if isinstance(
                statement, (ast.ClassDef, ast.FunctionDef, ast.AsyncFunctionDef)
            )
            and statement.name == object_name
        ),
        None,
    )


def property_mutator(method: ast.FunctionDef | ast.AsyncFunctionDef) -> bool:
    return any(
        isinstance(decorator, ast.Attribute)
        and decorator.attr in {"setter", "deleter"}
        for decorator in method.decorator_list
    )


def check_python_api(root: Path) -> tuple[int, int]:
    baseline_path = root / "manifests/api/python-api-v1.json"
    docs_path = root / "docs/reference/python-api.json"
    try:
        baseline = load_json(baseline_path)
        documentation = load_json(docs_path)
        exports, groups = validate(baseline, documentation)
    except PythonDocumentationError as error:
        raise DocumentationCheckError(str(error)) from error

    init_path = root / "python/src/postgamma/__init__.py"
    module = ast.parse(init_path.read_text(encoding="utf-8"), filename=str(init_path))
    module_exports = public_all(module)
    if module_exports != exports:
        raise DocumentationCheckError(
            "postgamma.__all__ differs from the frozen documentation baseline"
        )
    origins = object_origins(module)
    rendered_objects = {
        member["name"]
        for group in groups
        for member in group["members"]
        if member["render"] == "object"
    }
    rendered_members = {
        member["name"]: member
        for group in groups
        for member in group["members"]
        if member["render"] == "object"
    }
    undocumented: list[str] = []
    undocumented_members: list[str] = []
    hidden_inherited_members: list[str] = []
    incomplete_docstrings: list[str] = []
    unresolved: list[str] = []
    for name in sorted(rendered_objects):
        origin = origins.get(name)
        if origin is None:
            unresolved.append(name)
            continue
        node = source_object(root, *origin)
        if node is None:
            unresolved.append(name)
        elif ast.get_docstring(node) is None:
            undocumented.append(name)
        elif isinstance(node, ast.ClassDef):
            for member in node.body:
                if (
                    isinstance(member, (ast.FunctionDef, ast.AsyncFunctionDef))
                    and not member.name.startswith("_")
                    and not property_mutator(member)
                    and ast.get_docstring(member) is None
                ):
                    undocumented_members.append(f"{name}.{member.name}")
            for base in node.bases:
                if not isinstance(base, ast.Name) or not base.id.startswith("_"):
                    continue
                base_node = source_object(root, origin[0], base.id)
                if not isinstance(base_node, ast.ClassDef):
                    continue
                public_inherited = [
                    member
                    for member in base_node.body
                    if isinstance(member, (ast.FunctionDef, ast.AsyncFunctionDef))
                    and not member.name.startswith("_")
                    and not property_mutator(member)
                ]
                if not public_inherited:
                    continue
                if not rendered_members[name].get("inherited_members", False):
                    hidden_inherited_members.extend(
                        f"{name}.{member.name}" for member in public_inherited
                    )
                    continue
                undocumented_members.extend(
                    f"{name}.{member.name}"
                    for member in public_inherited
                    if ast.get_docstring(member) is None
                )
    for qualified_name, sections in STRUCTURED_PYTHON_DOCSTRINGS.items():
        object_name, separator, method_name = qualified_name.partition(".")
        origin = origins.get(object_name)
        node = None if origin is None else source_object(root, *origin)
        if separator:
            node = (
                next(
                    (
                        member
                        for member in node.body
                        if isinstance(
                            member, (ast.FunctionDef, ast.AsyncFunctionDef)
                        )
                        and member.name == method_name
                    ),
                    None,
                )
                if isinstance(node, ast.ClassDef)
                else None
            )
        docstring = ast.get_docstring(node) if node is not None else None
        missing_sections = [
            section for section in sections if docstring is None or section not in docstring
        ]
        if missing_sections:
            incomplete_docstrings.append(
                f"{qualified_name}:{','.join(missing_sections)}"
            )
    if (
        unresolved
        or undocumented
        or undocumented_members
        or hidden_inherited_members
        or incomplete_docstrings
    ):
        raise DocumentationCheckError(
            "Python public objects are not reference-ready: "
            f"unresolved={unresolved}, undocumented={undocumented}, "
            f"undocumented_members={undocumented_members}, "
            f"hidden_inherited_members={hidden_inherited_members}, "
            f"incomplete_docstrings={incomplete_docstrings}"
        )
    return len(exports), len(groups)


def level_one_headings(markdown: str) -> list[str]:
    """Return prose level-one headings while ignoring fenced code."""

    headings: list[str] = []
    fence_character: str | None = None
    fence_length = 0
    for line in markdown.splitlines():
        match = FENCE.match(line)
        if match is not None:
            token = match.group(1)
            if fence_character is None:
                fence_character = token[0]
                fence_length = len(token)
            elif token[0] == fence_character and len(token) >= fence_length:
                fence_character = None
                fence_length = 0
            continue
        if fence_character is None and line.startswith("# "):
            headings.append(line[2:].strip())
    return headings


def check_markdown(root: Path) -> int:
    missing = [path for path in REQUIRED_DOCUMENTS if not (root / path).is_file()]
    if missing:
        raise DocumentationCheckError(
            "required public document(s) are missing: " + ", ".join(missing)
        )
    documents = sorted((root / "docs").rglob("*.md"))
    for path in documents:
        text = path.read_text(encoding="utf-8")
        if not text.startswith("# "):
            raise DocumentationCheckError(
                f"{path.relative_to(root)} must begin with one level-one title"
            )
        headings = level_one_headings(text)
        if len(headings) != 1:
            raise DocumentationCheckError(
                f"{path.relative_to(root)} must contain exactly one prose "
                f"level-one title, found {len(headings)}"
            )
    for marker, relative in REFERENCE_MARKERS.items():
        occurrences = [
            path.relative_to(root)
            for path in documents
            if marker in path.read_text(encoding="utf-8")
        ]
        if occurrences != [Path(relative)]:
            raise DocumentationCheckError(
                f"reference marker {marker} must occur only in {relative}: "
                f"{occurrences}"
            )
    return len(documents)


def python_example_manifest(root: Path) -> list[dict[str, str]]:
    document = read_json(root / PYTHON_EXAMPLE_MANIFEST)
    if (
        document.get("schema_version") != 1
        or document.get("kind") != "postgamma.python-documentation-examples"
        or not isinstance(document.get("examples"), list)
        or not document["examples"]
    ):
        raise DocumentationCheckError("invalid Python example manifest")
    examples: list[dict[str, str]] = []
    for entry in document["examples"]:
        if (
            not isinstance(entry, dict)
            or set(entry) != {"name", "path", "stdout"}
            or not all(
                isinstance(entry.get(field), str) and entry[field]
                for field in ("name", "path")
            )
            or not isinstance(entry.get("stdout"), str)
        ):
            raise DocumentationCheckError("invalid Python example manifest entry")
        relative = Path(entry["path"])
        if relative.name != entry["path"] or relative.suffix != ".py":
            raise DocumentationCheckError(
                f"unsafe Python example path: {entry['path']}"
            )
        examples.append(entry)
    names = [entry["name"] for entry in examples]
    paths = [entry["path"] for entry in examples]
    if len(names) != len(set(names)) or len(paths) != len(set(paths)):
        raise DocumentationCheckError("Python example manifest contains duplicates")
    discovered = sorted(
        path.name for path in (root / "examples/python").glob("*.py")
    )
    if sorted(paths) != discovered:
        raise DocumentationCheckError(
            "Python example manifest is not exact: "
            f"missing={sorted(set(discovered) - set(paths))}, "
            f"stale={sorted(set(paths) - set(discovered))}"
        )
    return examples


def check_examples(root: Path) -> list[str]:
    examples = python_example_manifest(root)
    for entry in examples:
        path = root / "examples/python" / entry["path"]
        source = path.read_text(encoding="utf-8")
        try:
            compile(source, str(path), "exec")
        except SyntaxError as error:
            raise DocumentationCheckError(
                f"invalid Python documentation example {path}: {error}"
            ) from error
    return [entry["name"] for entry in examples]


def check_agent_index(root: Path) -> int:
    """Validate the public machine-readable documentation index."""

    path = root / "docs/llms.txt"
    if not path.is_file():
        raise DocumentationCheckError("docs/llms.txt is missing")
    content = path.read_text(encoding="utf-8")
    if not content.startswith("# PostGamma\n"):
        raise DocumentationCheckError("docs/llms.txt has an invalid title")
    if "/home/" in content or "/build/" in content:
        raise DocumentationCheckError(
            "docs/llms.txt contains a developer-local or build path"
        )
    targets = re.findall(r"\[[^]]+\]\(([^)]+)\)", content)
    if not targets or len(targets) != len(set(targets)):
        raise DocumentationCheckError(
            "docs/llms.txt links must be nonempty and unique"
        )
    missing: list[str] = []
    for target in targets:
        if "://" in target or target.startswith("/") or "#" in target:
            raise DocumentationCheckError(
                f"docs/llms.txt link is not a canonical relative page: {target}"
            )
        logical = target.rstrip("/")
        page = root / "docs" / f"{logical}.md"
        section = root / "docs" / logical / "index.md"
        if not page.is_file() and not section.is_file():
            missing.append(target)
    if missing:
        raise DocumentationCheckError(
            "docs/llms.txt links missing source pages: " + ", ".join(missing)
        )
    return len(targets)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", required=True, type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    root = args.root.resolve()
    try:
        c_functions, extension_functions = check_c_api(root)
        python_exports, python_groups = check_python_api(root)
        markdown_files = check_markdown(root)
        python_examples = check_examples(root)
        agent_index_links = check_agent_index(root)
    except DocumentationCheckError as error:
        parser.error(str(error))
    report = {
        "schema_version": 1,
        "kind": "postgamma.documentation-evidence",
        "status": "pass",
        "c_function_count": c_functions,
        "extension_function_count": extension_functions,
        "python_export_count": python_exports,
        "python_group_count": python_groups,
        "markdown_file_count": markdown_files,
        "python_example_count": len(python_examples),
        "python_examples": python_examples,
        "agent_index_link_count": agent_index_links,
    }
    if args.output is not None:
        output = args.output.resolve()
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_text(
            json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8"
        )
    print(
        "documentation: "
        f"{c_functions} C functions, {extension_functions} extension functions, "
        f"{python_exports} Python exports, {markdown_files} Markdown pages"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
