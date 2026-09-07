"""Tests for fail-closed private-libpq source hook preparation."""

from __future__ import annotations

import copy
import sys
import unittest
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(PROJECT_ROOT / "buildsys"))

import postgresql_embedded_adapter  # noqa: E402
import prepare_private_libpq  # noqa: E402
import check_private_libpq  # noqa: E402


class PreparePrivateLibpqTests(unittest.TestCase):
    def test_current_pg19_hooks_are_exact_and_function_bounded(self) -> None:
        adapter = postgresql_embedded_adapter.load_adapter(
            PROJECT_ROOT / "manifests/postgresql/embedded.json"
        )
        hooks_by_file: dict[str, list[dict[str, object]]] = {}
        for hook in adapter["libpq_memory_hooks"]:
            hooks_by_file.setdefault(hook["source_file"], []).append(hook)
        total = 0
        for relative, hooks in hooks_by_file.items():
            source = (PROJECT_ROOT / "postgres" / relative).read_text(
                encoding="utf-8"
            )
            transformed, facts = prepare_private_libpq.transform_file(source, hooks)
            self.assertNotEqual(source, transformed)
            total += len(facts)
        self.assertEqual(total, 7)

    def test_source_report_contract_is_derived_from_the_adapter(self) -> None:
        adapter = postgresql_embedded_adapter.load_adapter(
            PROJECT_ROOT / "manifests/postgresql/embedded.json"
        )
        files: dict[str, list[dict[str, str]]] = {}
        for hook in adapter["libpq_memory_hooks"]:
            files.setdefault(hook["source_file"], []).append({"id": hook["id"]})
        report = {
            "schema_version": 1,
            "kind": "postgamma.private-libpq-source",
            "adapter_id": adapter["id"],
            "hook_count": len(adapter["libpq_memory_hooks"]),
            "files": [
                {"source_file": source, "hooks": hooks}
                for source, hooks in sorted(files.items())
            ],
        }
        self.assertEqual(
            check_private_libpq.validate_source_report(report, adapter), 7
        )

        incomplete = copy.deepcopy(report)
        incomplete["files"][0]["hooks"].clear()
        with self.assertRaisesRegex(
            check_private_libpq.PrivateLibpqCheckError,
            "provider seam set",
        ):
            check_private_libpq.validate_source_report(incomplete, adapter)

    def test_replace_hook_fails_closed_on_anchor_drift(self) -> None:
        hook = {
            "id": "guard",
            "source_file": "fixture.c",
            "enclosing_function": "probe",
            "expected_definitions": 1,
            "kind": "replace_once",
            "anchor": "missing",
            "replacement": "POSTGAMMA_LIBPQ_MEMORY_BOUND(conn)",
        }
        with self.assertRaisesRegex(
            prepare_private_libpq.PrivateLibpqSourceError, "found 0"
        ):
            prepare_private_libpq.transform_file(
                "int\nprobe(void)\n{\n\treturn 0;\n}\n", [hook]
            )


if __name__ == "__main__":
    unittest.main()
