from __future__ import annotations

import json
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "buildsys"))

from bundle_embedded_static_modules import (  # noqa: E402
    StaticModuleBundleError,
    bundle,
    global_definitions,
    read_json,
    render_facts,
    validate_manifest,
    validate_link_database,
)


class StaticModuleManifestV2Tests(unittest.TestCase):
    def setUp(self) -> None:
        self.document = read_json(
            ROOT / "manifests/extensions/embedded-static.json"
        )

    def probe(self) -> dict[str, object]:
        return next(
            module
            for module in self.document["modules"]
            if module["id"] == "postgamma_sdk_probe"
        )

    def vector(self) -> dict[str, object]:
        return next(
            module
            for module in self.document["modules"]
            if module["id"] == "pgvector"
        )

    def test_generated_c_escapes_manifest_strings(self) -> None:
        self.probe()["version"] = '1.0"quoted\\value'
        rendered = render_facts(validate_manifest(self.document))
        self.assertIn('"1.0\\"quoted\\\\value"', rendered)

    def test_rejects_noncanonical_resource_paths(self) -> None:
        self.probe()["resources"][0]["logical_path"] = "share//extension/file"
        with self.assertRaisesRegex(StaticModuleBundleError, "safe relative path"):
            validate_manifest(self.document)

    def test_rejects_an_unsupported_sdk_capability(self) -> None:
        self.probe()["capabilities"]["filesystem-write"] = True
        with self.assertRaisesRegex(StaticModuleBundleError, "unsupported"):
            validate_manifest(self.document)

    def test_accepts_an_sdk_module_without_session_callbacks(self) -> None:
        manifest = validate_manifest(self.document)
        vector = next(
            module for module in manifest["modules"] if module["id"] == "pgvector"
        )
        self.assertEqual(
            vector["lifecycle"],
            [
                "instance-request",
                "instance-shutdown",
                "instance-startup",
                "library-initialize",
            ],
        )


@unittest.skipUnless(
    all(shutil.which(tool) for tool in ("cc", "objcopy", "nm")),
    "ELF build tools are required",
)
class EmbeddedStaticModuleTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def write_json(self, name: str, document: dict[str, object]) -> Path:
        path = self.root / name
        path.write_text(
            json.dumps(document, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
        return path

    def compile(self, name: str, source: str) -> Path:
        source_path = self.root / f"{name}.c"
        object_path = self.root / f"{name}.o"
        source_path.write_text(source, encoding="utf-8")
        subprocess.run(
            ["cc", "-fPIC", "-c", str(source_path), "-o", str(object_path)],
            check=True,
        )
        return object_path

    def manifest(self, symbols: list[dict[str, str]]) -> dict[str, object]:
        return {
            "schema_version": 1,
            "kind": "postgamma.embedded-static-module-manifest",
            "id": "fixture-modules",
            "modules": [
                {
                    "id": "fixture",
                    "logical_name": "fixture",
                    "link_output_suffix": "modules/fixture.so",
                    "symbols": symbols,
                }
            ],
        }

    def link_database(self, object_path: Path) -> dict[str, object]:
        return {
            "schema_version": 1,
            "kind": "postgamma.link-command-database",
            "build_root": str(self.root),
            "identity": {"fixture": "current"},
            "commands": [
                {
                    "output": str(self.root / "modules" / "fixture.so"),
                    "ordered_inputs": [
                        {"kind": "object", "path": str(object_path)}
                    ],
                }
            ],
        }

    def test_bundles_registered_symbols_and_localizes_everything_else(self) -> None:
        object_path = self.compile(
            "fixture",
            "int hidden_helper(void) { return 7; }\n"
            "int Pg_magic_func(void) { return 19; }\n"
            "int fixture_entry(void) { return hidden_helper(); }\n",
        )
        symbols = [
            {
                "logical_name": "Pg_magic_func",
                "linker_name": "postgamma_module_fixture_magic",
            },
            {
                "logical_name": "fixture_entry",
                "linker_name": "postgamma_module_fixture_entry",
            },
        ]
        manifest = self.write_json("manifest.json", self.manifest(symbols))
        database = self.write_json(
            "link-database.json", self.link_database(object_path)
        )
        output = self.root / "bundle" / "modules.o"
        facts = self.root / "bundle" / "modules.inc"
        receipt = self.root / "bundle" / "receipt.json"
        document = bundle(
            manifest,
            database,
            "cc",
            "objcopy",
            "nm",
            output,
            facts,
            receipt,
        )
        self.assertEqual(
            global_definitions("nm", output),
            [
                "postgamma_module_fixture_entry",
                "postgamma_module_fixture_magic",
            ],
        )
        self.assertEqual(document["modules"][0]["localized_symbol_count"], 1)
        self.assertNotIn("hidden_helper", facts.read_text(encoding="utf-8"))
        self.assertTrue(receipt.is_file())

    def test_rejects_a_registered_symbol_missing_from_the_link_closure(self) -> None:
        object_path = self.compile(
            "fixture", "int Pg_magic_func(void) { return 19; }\n"
        )
        symbols = [
            {
                "logical_name": "Pg_magic_func",
                "linker_name": "postgamma_module_fixture_magic",
            },
            {
                "logical_name": "fixture_entry",
                "linker_name": "postgamma_module_fixture_entry",
            },
        ]
        manifest = self.write_json("manifest.json", self.manifest(symbols))
        database = self.write_json(
            "link-database.json", self.link_database(object_path)
        )
        with self.assertRaisesRegex(StaticModuleBundleError, "missing registered"):
            bundle(
                manifest,
                database,
                "cc",
                "objcopy",
                "nm",
                self.root / "bundle" / "modules.o",
                self.root / "bundle" / "modules.inc",
                self.root / "bundle" / "receipt.json",
            )

    def test_rejects_the_wrong_link_database_identity(self) -> None:
        with self.assertRaisesRegex(StaticModuleBundleError, "identity"):
            validate_link_database(
                {
                    "schema_version": 1,
                    "kind": "wrong",
                    "build_root": str(self.root),
                    "identity": {},
                    "commands": [],
                }
            )


if __name__ == "__main__":
    unittest.main()
