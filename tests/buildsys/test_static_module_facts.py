"""Tests for static native-extension manifest and fact generation."""

from __future__ import annotations

import json
import sys
import tempfile
import unittest
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(PROJECT_ROOT / "buildsys"))

import generate_static_module_facts  # noqa: E402


class StaticModuleFactsTests(unittest.TestCase):
    manifest_path = PROJECT_ROOT / "manifests/extensions/bootstrap-static-extension.json"

    def test_real_manifest_generates_only_declarations_and_initializers(self) -> None:
        manifest = generate_static_module_facts.validate_manifest(
            generate_static_module_facts.load_json(self.manifest_path),
            PROJECT_ROOT,
        )
        rendered = generate_static_module_facts.render_facts(manifest)
        self.assertIn("postgamma_generated_registry", rendered)
        self.assertIn("POSTGAMMA_STATIC_FUNCTION(pgm_bootstrap_extension_magic)", rendered)
        self.assertNotIn("void\n", rendered)
        self.assertNotIn("return ", rendered)
        logical_names = [
            symbol["logical_name"] for symbol in manifest["modules"][0]["symbols"]
        ]
        self.assertEqual(logical_names, sorted(logical_names))

    def test_real_manifest_report_hashes_every_tracked_resource(self) -> None:
        manifest = generate_static_module_facts.validate_manifest(
            generate_static_module_facts.load_json(self.manifest_path),
            PROJECT_ROOT,
        )
        report = generate_static_module_facts.facts_report(
            manifest, self.manifest_path, PROJECT_ROOT
        )
        self.assertEqual(report["module_count"], 1)
        self.assertEqual(report["symbol_count"], 6)
        self.assertEqual(len(report["resources"]), 4)

    def test_manifest_rejects_an_unknown_field(self) -> None:
        document = generate_static_module_facts.load_json(self.manifest_path)
        document["typo"] = True
        with self.assertRaisesRegex(
            generate_static_module_facts.StaticModuleFactsError, "unknown field"
        ):
            generate_static_module_facts.validate_manifest(document, PROJECT_ROOT)

    def test_manifest_requires_module_magic(self) -> None:
        document = generate_static_module_facts.load_json(self.manifest_path)
        document["modules"][0]["symbols"] = [
            symbol
            for symbol in document["modules"][0]["symbols"]
            if symbol["logical_name"] != "Pg_magic_func"
        ]
        with self.assertRaisesRegex(
            generate_static_module_facts.StaticModuleFactsError, "Pg_magic_func"
        ):
            generate_static_module_facts.validate_manifest(document, PROJECT_ROOT)

    def test_manifest_rejects_a_missing_resource(self) -> None:
        document = generate_static_module_facts.load_json(self.manifest_path)
        document["modules"][0]["sql"] = "tests/embedded/bootstrap/missing.sql"
        with self.assertRaisesRegex(
            generate_static_module_facts.StaticModuleFactsError, "does not exist"
        ):
            generate_static_module_facts.validate_manifest(document, PROJECT_ROOT)

    def test_json_loader_rejects_duplicate_keys(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "duplicate.json"
            path.write_text('{"schema_version": 1, "schema_version": 1}\n')
            with self.assertRaisesRegex(
                generate_static_module_facts.StaticModuleFactsError,
                "duplicate JSON key",
            ):
                generate_static_module_facts.load_json(path)

    def test_generated_bytes_are_deterministic(self) -> None:
        document = json.loads(self.manifest_path.read_text(encoding="utf-8"))
        manifest = generate_static_module_facts.validate_manifest(
            document, PROJECT_ROOT
        )
        first = generate_static_module_facts.render_facts(manifest)
        document["modules"][0]["symbols"].reverse()
        reordered = generate_static_module_facts.validate_manifest(
            document, PROJECT_ROOT
        )
        self.assertEqual(first, generate_static_module_facts.render_facts(reordered))


if __name__ == "__main__":
    unittest.main()
