"""Tests for declarative PostgreSQL compile-domain selection."""

from __future__ import annotations

import sys
import tempfile
import unittest
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(PROJECT_ROOT / "buildsys"))

import filter_compile_database  # noqa: E402


class CompileDatabaseDomainTests(unittest.TestCase):
    @staticmethod
    def manifest(**changes: object) -> dict[str, object]:
        document: dict[str, object] = {
            "schema_version": 1,
            "kind": filter_compile_database.DOMAIN_KIND,
            "id": "fixture-frontend",
            "description": "Fixture frontend compile domain",
            "translation_unit_prefixes": ["src/common/"],
            "translation_unit_files": [],
            "required_defines": ["FRONTEND"],
            "required_any_defines": [],
            "excluded_defines": [],
            "required_output_suffixes": [],
            "excluded_output_suffixes": ["_shlib.o", "_srv.o"],
            "binding_anchor_file_suffixes": [],
        }
        document.update(changes)
        return document

    @staticmethod
    def record(source: Path, output: str, *defines: str) -> dict[str, object]:
        return {
            "directory": str(source.parent),
            "file": str(source),
            "arguments": [
                "cc",
                *(f"-D{define}" for define in defines),
                "-c",
                source.name,
                "-o",
                output,
            ],
        }

    def test_positive_define_and_output_contract_select_one_profile(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary) / "postgres"
            source = root / "src/common/sample.c"
            source.parent.mkdir(parents=True)
            source.write_text(
                '#ifndef FRONTEND\n#include "postgres.h"\n#else\n'
                '#include "postgres_fe.h"\n#endif\n',
                encoding="utf-8",
            )
            records = [
                self.record(source, "sample_srv.o"),
                self.record(source, "sample_shlib.o", "FRONTEND"),
                self.record(source, "sample.o", "FRONTEND"),
            ]
            selected, report = filter_compile_database.select_compile_records(
                records, root.resolve(), None, self.manifest()
            )
        self.assertEqual(len(selected), 1)
        self.assertTrue(selected[0]["arguments"][-1].endswith("sample.o"))
        self.assertEqual(report["domain"], "fixture-frontend")
        self.assertEqual(report["selected_translation_units"], 1)

    def test_frontend_umbrella_header_satisfies_required_define(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary) / "postgres"
            source = root / "src/common/frontend.c"
            source.parent.mkdir(parents=True)
            source.write_text('#include "postgres_fe.h"\n', encoding="utf-8")
            selected, _report = filter_compile_database.select_compile_records(
                [self.record(source, "frontend.o")],
                root.resolve(),
                None,
                self.manifest(),
            )
        self.assertEqual(len(selected), 1)

    def test_two_remaining_profiles_are_rejected_as_ambiguous(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary) / "postgres"
            source = root / "src/common/sample.c"
            source.parent.mkdir(parents=True)
            source.write_text('#include "postgres_fe.h"\n', encoding="utf-8")
            records = [
                self.record(source, "first.o"),
                self.record(source, "second.o"),
            ]
            with self.assertRaisesRegex(ValueError, "fixture-frontend"):
                filter_compile_database.select_compile_records(
                    records, root.resolve(), None, self.manifest()
                )

    def test_manifest_rejects_unknown_fields(self) -> None:
        with self.assertRaisesRegex(ValueError, "unknown field"):
            filter_compile_database.parse_domain_manifest(
                self.manifest(typo=True)
            )

    def test_manifest_rejects_conflicting_define_contract(self) -> None:
        with self.assertRaisesRegex(ValueError, "requires and excludes"):
            filter_compile_database.parse_domain_manifest(
                self.manifest(excluded_defines=["FRONTEND"])
            )

    def test_required_any_define_supports_frontend_tool_dependencies(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary) / "postgres"
            source = root / "src/common/generic.c"
            source.parent.mkdir(parents=True)
            source.write_text('#include "c.h"\n', encoding="utf-8")
            selected, _report = filter_compile_database.select_compile_records(
                [self.record(source, "generic.o", "USE_PRIVATE_ENCODING_FUNCS")],
                root.resolve(),
                None,
                self.manifest(
                    required_defines=[],
                    required_any_defines=[
                        "FRONTEND",
                        "USE_PRIVATE_ENCODING_FUNCS",
                    ],
                ),
            )
        self.assertEqual(len(selected), 1)

    def test_compiler_output_accepts_joined_output_argument(self) -> None:
        directory = Path("/tmp/postgamma-output-fixture")
        self.assertEqual(
            filter_compile_database.compiler_output(
                ["cc", "-c", "sample.c", "-osample.o"], directory
            ),
            (directory / "sample.o").resolve(),
        )

    def test_compile_record_requires_an_explicit_directory(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary) / "postgres"
            source = root / "src/common/frontend.c"
            source.parent.mkdir(parents=True)
            source.write_text('#include "postgres_fe.h"\n', encoding="utf-8")
            record = self.record(source, "frontend.o")
            del record["directory"]
            with self.assertRaisesRegex(ValueError, "directory must be"):
                filter_compile_database.select_compile_records(
                    [record], root.resolve(), None, self.manifest()
                )


if __name__ == "__main__":
    unittest.main()
