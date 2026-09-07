"""Tests for public documentation generation and fail-closed coverage."""

from __future__ import annotations

import copy
import json
import os
import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest import mock


PROJECT_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(PROJECT_ROOT / "buildsys"))

import check_documentation  # noqa: E402
import generate_c_api_docs  # noqa: E402
import generate_python_api_docs  # noqa: E402
import mkdocs_hooks  # noqa: E402


class DocumentationTests(unittest.TestCase):
    def test_repository_documentation_covers_every_public_api(self) -> None:
        self.assertEqual(check_documentation.check_c_api(PROJECT_ROOT), (75, 6))
        self.assertEqual(
            check_documentation.check_python_api(PROJECT_ROOT), (74, 8)
        )
        self.assertGreaterEqual(
            check_documentation.check_markdown(PROJECT_ROOT),
            len(check_documentation.REQUIRED_DOCUMENTS),
        )
        self.assertEqual(
            check_documentation.check_examples(PROJECT_ROOT),
            [
                "quickstart",
                "async-concurrency",
                "backup-restore",
                "vector-search",
            ],
        )
        self.assertGreater(
            check_documentation.check_agent_index(PROJECT_ROOT), 0
        )

    def test_python_inventory_rejects_a_missing_export(self) -> None:
        baseline = generate_python_api_docs.load_json(
            PROJECT_ROOT / "manifests/api/python-api-v1.json"
        )
        inventory = generate_python_api_docs.load_json(
            PROJECT_ROOT / "docs/reference/python-api.json"
        )
        incomplete = copy.deepcopy(inventory)
        incomplete["groups"][0]["members"].pop()
        with self.assertRaisesRegex(
            generate_python_api_docs.DocumentationError, "missing="
        ):
            generate_python_api_docs.validate(baseline, incomplete)

    def test_python_example_manifest_rejects_unlisted_source(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            examples = root / "examples/python"
            manifest = root / "docs/reference"
            examples.mkdir(parents=True)
            manifest.mkdir(parents=True)
            (examples / "listed.py").write_text("print('listed')\n", encoding="utf-8")
            (examples / "unlisted.py").write_text(
                "print('unlisted')\n", encoding="utf-8"
            )
            (manifest / "python-examples.json").write_text(
                json.dumps(
                    {
                        "schema_version": 1,
                        "kind": "postgamma.python-documentation-examples",
                        "examples": [
                            {
                                "name": "listed",
                                "path": "listed.py",
                                "stdout": "listed\n",
                            }
                        ],
                    }
                ),
                encoding="utf-8",
            )
            with self.assertRaisesRegex(
                check_documentation.DocumentationCheckError,
                "manifest is not exact",
            ):
                check_documentation.check_examples(root)

    def test_python_reference_renders_every_object_once(self) -> None:
        baseline = generate_python_api_docs.load_json(
            PROJECT_ROOT / "manifests/api/python-api-v1.json"
        )
        inventory = generate_python_api_docs.load_json(
            PROJECT_ROOT / "docs/reference/python-api.json"
        )
        _, groups = generate_python_api_docs.validate(baseline, inventory)
        rendered = generate_python_api_docs.render(groups)
        object_count = sum(
            member["render"] == "object"
            for group in groups
            for member in group["members"]
        )
        self.assertEqual(rendered.count("\n::: postgamma."), object_count)

    def test_markdown_title_check_ignores_python_comments_in_fences(self) -> None:
        markdown = "# Guide\n\n```python\n# create=True is the default\n```\n"
        self.assertEqual(check_documentation.level_one_headings(markdown), ["Guide"])

    def test_function_pointer_typedef_arguments_are_not_duplicated(self) -> None:
        document = """\
<doxygen>
  <compounddef>
    <sectiondef>
      <memberdef kind="typedef">
        <definition>typedef int(* pgmex_callback) (void)</definition>
        <argsstring>(void)</argsstring>
        <name>pgmex_callback</name>
      </memberdef>
    </sectiondef>
  </compounddef>
</doxygen>
"""
        with tempfile.TemporaryDirectory() as temporary:
            xml = Path(temporary)
            (xml / "header.xml").write_text(document, encoding="utf-8")
            typedefs = generate_c_api_docs.collect_typedefs(xml, "pgmex_")
        self.assertEqual(
            typedefs["pgmex_callback"],
            "typedef int(* pgmex_callback) (void);",
        )

    def test_c_macro_coverage_rejects_an_unclassified_public_macro(self) -> None:
        registry = [{"group": "status", "values": {"PGM_STATUS_OK": 0}}]
        structures = {"pgm_options": [("struct_size", "uint32_t")]}
        defines = {
            "PGM_STATUS_OK": "INT32_C(0)",
            "PGM_OPTIONS_INIT": "{sizeof(pgm_options)}",
            "PGM_ABI_VERSION_ENCODE": "value",
            "PGM_API": "",
            "PGM_NO_TIMEOUT": "INT64_C(-1)",
            "PGM_NEW_UNDOCUMENTED_MACRO": "1",
        }
        with self.assertRaisesRegex(
            generate_c_api_docs.DocumentationError, "unclassified=.*PGM_NEW"
        ):
            generate_c_api_docs.validate_host_macros(
                registry, structures, defines
            )

    def test_mkdocs_hook_uses_the_configured_generated_directory(self) -> None:
        marker = "<!-- POSTGAMMA_GENERATED_C_API -->"
        with tempfile.TemporaryDirectory() as temporary:
            generated = Path(temporary)
            (generated / "c-api.md").write_text("Generated reference.\n")
            config = SimpleNamespace(
                config_file_path=str(PROJECT_ROOT / "mkdocs.yml")
            )
            with mock.patch.dict(
                os.environ,
                {"POSTGAMMA_DOCS_GENERATED_DIR": str(generated)},
            ):
                result = mkdocs_hooks.on_page_markdown(
                    "# Reference\n\n" + marker + "\n",
                    None,
                    config,
                    None,
                )
        self.assertNotIn(marker, result)
        self.assertIn("Generated reference.", result)

    def test_mkdocs_hook_resolves_public_release_identity(self) -> None:
        config = SimpleNamespace(
            config_file_path=str(PROJECT_ROOT / "mkdocs.yml")
        )
        with mock.patch.dict(
            os.environ,
            {"POSTGAMMA_RELEASES_URL": "https://example.test/releases"},
        ):
            result = mkdocs_hooks.on_page_markdown(
                "Version {{ POSTGAMMA_VERSION }}; "
                "state {{ POSTGAMMA_RELEASE_STATE }}; "
                "releases {{ POSTGAMMA_RELEASES_URL }}.\n",
                None,
                config,
                None,
            )
        self.assertEqual(
            result,
            "Version 0.1.0a1; state alpha; "
            "releases https://example.test/releases.\n",
        )

    def test_c_documentation_inventory_is_valid_json(self) -> None:
        inventory = json.loads(
            (PROJECT_ROOT / "docs/reference/c-api.json").read_text(
                encoding="utf-8"
            )
        )
        self.assertEqual(inventory["kind"], "postgamma.c-api-documentation")


if __name__ == "__main__":
    unittest.main()
