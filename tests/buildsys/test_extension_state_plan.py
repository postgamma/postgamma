from __future__ import annotations

import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "buildsys"))

from generate_extension_state_plan import (  # noqa: E402
    ExtensionStatePlanError,
    compile_plan,
)


class ExtensionStatePlanTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.source = (
            "int extension_state;\n"
            "extern int core_state;\n"
            "extern int work_mem;\n"
            "int use_state(void)\n"
            "{\n"
            "    return extension_state + core_state + work_mem;\n"
            "}\n"
        )
        (self.root / "fixture.c").write_text(self.source, encoding="utf-8")
        self.extension_offset = self.source.rindex("extension_state")
        self.core_offset = self.source.rindex("core_state")
        self.guc_offset = self.source.rindex("work_mem")

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def catalog(self) -> dict[str, object]:
        def use(name: str, offset: int) -> dict[str, object]:
            return {
                "id": f"external:{name}",
                "name": name,
                "usr": f"c:@{name}",
                "path": "fixture.c",
                "translation_unit": "fixture.c",
                "source_kind": "source",
                "offset": offset,
                "length": len(name),
                "line": 6,
                "column": 12,
                "use_kind": "read",
                "macro_kind": "none",
                "in_static_initializer": False,
                "static_initializer_owner_id": "",
            }

        return {
            "schema_version": 1,
            "kind": "postgamma.backend-mutable-state-catalog",
            "candidates": [
                {
                    "id": "external:extension_state",
                    "name": "extension_state",
                    "usr": "c:@extension_state",
                    "definition_path": "fixture.c",
                    "canonical_type": "int",
                    "complete_type": True,
                    "trivially_copyable": True,
                    "size": 4,
                    "alignment": 4,
                }
            ],
            "uses": [use("extension_state", self.extension_offset)],
            "external_references": [
                use("core_state", self.core_offset),
                use("work_mem", self.guc_offset),
            ],
            "summary": {"candidate_count": 1},
        }

    @staticmethod
    def policy() -> dict[str, object]:
        return {
            "schema_version": 1,
            "kind": "postgamma.extension-state-ownership",
            "extension_id": "fixture",
            "accessor": "fixture_role_state_address",
            "upstream": {
                "repository": "https://example.invalid/fixture.git",
                "commit": "0123456789abcdef",
                "version": "1.0",
            },
            "decisions": [
                {
                    "id": "external:extension_state",
                    "owner": "role",
                    "rationale": "The value belongs to one logical backend role",
                }
            ],
        }

    @staticmethod
    def backend_policy() -> dict[str, object]:
        return {
            "schema_version": 1,
            "kind": "postgamma.backend-state-ownership",
            "decisions": [
                {
                    "id": "external:core_state",
                    "owner": "role",
                    "rationale": "The kernel owns this role-local value",
                },
                {
                    "id": "external:work_mem",
                    "owner": "managed_guc",
                    "rationale": "The kernel GUC pipeline owns this value",
                },
            ],
        }

    @staticmethod
    def backend_runtime() -> dict[str, object]:
        return {
            "schema_version": 1,
            "kind": "postgamma.backend-state-runtime",
            "slots": [
                {
                    "id": "external:core_state",
                    "owner": "role",
                    "enum": "POSTGAMMA_BACKEND_STATE_CORE_STATE",
                    "name": "core_state",
                    "relocations": [],
                }
            ],
        }

    @staticmethod
    def guc_manifest() -> dict[str, object]:
        return {
            "schema_version": 1,
            "kind": "postgamma.ast-transform",
            "symbols": [
                {
                    "name": "work_mem",
                    "replacement": "POSTGAMMA_GUC_VALUE(work_mem)",
                }
            ],
        }

    def compile(self) -> tuple[dict[str, object], dict[str, object]]:
        return compile_plan(
            self.catalog(),
            self.policy(),
            self.root,
            self.backend_policy(),
            self.backend_runtime(),
            self.guc_manifest(),
        )

    def test_virtualizes_extension_and_kernel_state_in_one_plan(self) -> None:
        plan, report = self.compile()
        summary = report["summary"]
        self.assertEqual(summary["candidate_count"], 1)
        self.assertEqual(summary["kernel_reference_count"], 2)
        self.assertEqual(summary["kernel_reference_symbol_count"], 2)
        self.assertEqual(summary["kernel_state_replacement_count"], 1)
        self.assertEqual(summary["kernel_guc_replacement_count"], 1)
        self.assertEqual(summary["replacement_count"], 3)
        replacements = plan["files"][0]["replacements"]
        self.assertEqual(
            {replacement["kind"] for replacement in replacements},
            {
                "extension_role_state",
                "kernel_backend_state",
                "kernel_guc_state",
            },
        )
        self.assertEqual(
            report["kernel_references"],
            ["external:core_state", "external:work_mem"],
        )

    def test_rejects_an_unclassified_kernel_reference(self) -> None:
        policy = self.backend_policy()
        policy["decisions"] = policy["decisions"][:1]
        with self.assertRaisesRegex(
            ExtensionStatePlanError, "has no kernel ownership decision"
        ):
            compile_plan(
                self.catalog(),
                self.policy(),
                self.root,
                policy,
                self.backend_runtime(),
                self.guc_manifest(),
            )

    def test_rejects_a_managed_guc_without_a_transform(self) -> None:
        manifest = self.guc_manifest()
        manifest["symbols"] = []
        with self.assertRaisesRegex(
            ExtensionStatePlanError, "managed GUC has no transform symbol"
        ):
            compile_plan(
                self.catalog(),
                self.policy(),
                self.root,
                self.backend_policy(),
                self.backend_runtime(),
                manifest,
            )


if __name__ == "__main__":
    unittest.main()
