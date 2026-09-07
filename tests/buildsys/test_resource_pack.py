from __future__ import annotations

import json
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]

import sys

sys.path.insert(0, str(ROOT / "buildsys"))

from build_resource_pack import (  # noqa: E402
    ResourcePackError,
    build_resource_pack,
    normalized_mode,
    validate_logical_path,
)


class ResourcePackTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.install = self.root / "install" / "usr" / "local" / "pgsql"
        (self.install / "bin").mkdir(parents=True)
        (self.install / "share").mkdir()
        (self.install / "bin" / "postgres").write_bytes(b"postgres-fixture\n")
        (self.install / "bin" / "postgres").chmod(0o755)
        (self.install / "share" / "postgres.bki").write_bytes(b"catalog\n")

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def build(self, name: str) -> tuple[Path, dict[str, object]]:
        output = self.root / name / "pack"
        receipt = self.root / name / "receipt.json"
        document = build_resource_pack(
            ROOT,
            self.root / "install",
            ROOT / "manifests/extensions/embedded-static.json",
            ROOT / "manifests/upstream.json",
            output,
            receipt,
            self.root,
        )
        return output, document

    def test_pack_is_deterministic_and_contains_declared_resources(self) -> None:
        first_root, first = self.build("first")
        second_root, second = self.build("second")
        self.assertEqual(first, second)
        self.assertEqual(first["tree_sha256"], second["tree_sha256"])
        self.assertTrue(
            (first_root / "share/extension/postgamma_sdk_probe.control").is_file()
        )
        self.assertTrue((first_root / "lib").is_dir())
        self.assertEqual(
            first["required_layout_directories"], ["bin", "lib", "share"]
        )
        self.assertEqual(
            (first_root / "share/postgamma/extensions/postgamma_sdk_probe/seed.txt")
            .read_text(encoding="utf-8")
            .strip(),
            "postgamma-sdk-probe-resource",
        )
        metadata = json.loads(
            (first_root / "share/postgamma/resource-pack.json").read_text(
                encoding="utf-8"
            )
        )
        self.assertEqual(metadata, first)
        self.assertNotIn(str(ROOT), json.dumps(first, sort_keys=True))

    def test_conflicting_installed_resource_fails_closed(self) -> None:
        extension = self.install / "share" / "extension"
        extension.mkdir()
        (extension / "plpgsql.control").write_bytes(b"wrong\n")
        with self.assertRaisesRegex(ResourcePackError, "conflicts"):
            self.build("conflict")

    def test_logical_path_contract_rejects_noncanonical_names(self) -> None:
        for value in ("/absolute", "a/../b", "a/./b", "a//b", "a/", "a\\b", "a\nb"):
            with self.subTest(value=value), self.assertRaises(ResourcePackError):
                validate_logical_path(value)

    def test_output_removal_is_bounded_by_an_explicit_root(self) -> None:
        outside = self.root.parent / f"{self.root.name}-outside"
        with self.assertRaisesRegex(ResourcePackError, "strict children"):
            build_resource_pack(
                ROOT,
                self.root / "install",
                ROOT / "manifests/extensions/embedded-static.json",
                ROOT / "manifests/upstream.json",
                outside / "pack",
                outside / "receipt.json",
                self.root,
            )
        self.assertFalse(outside.exists())

    def test_normalized_mode_depends_only_on_mode_bits(self) -> None:
        source = self.install / "share" / "mode-fixture"
        source.write_bytes(b"fixture\n")
        source.chmod(0o600)
        self.assertEqual(normalized_mode(source), 0o644)
        source.chmod(0o710)
        self.assertEqual(normalized_mode(source), 0o755)


if __name__ == "__main__":
    unittest.main()
