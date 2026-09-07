from __future__ import annotations

import json
import sys
import tempfile
import unittest
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(PROJECT_ROOT / "buildsys"))

import check_backend_state_policy  # noqa: E402
import install_generated_support  # noqa: E402
import postgresql_adapter  # noqa: E402
import propose_backend_state_policy  # noqa: E402


def adapter_document(major: int = 20) -> dict[str, object]:
    return {
        "schema_version": postgresql_adapter.ADAPTER_SCHEMA_VERSION,
        "kind": postgresql_adapter.ADAPTER_KIND,
        "id": "fixture",
        "compatibility_scope": "fixture",
        "supported_postgresql_majors": [major],
        "fact_sources": {
            "backend_execution_catalog": {
                "path": "src/proctypelist.h",
                "parser": "fixture.backend-execution.v1",
            },
            "guc_catalog": {
                "path": "src/gucs.dat",
                "parser": "fixture.gucs.v1",
            },
        },
        "backend_inheritance": {
            "source_file": "src/launch.c",
            "contract_function": "save_backend_variables",
            "extra_compile_arguments": ["-DEXEC_BACKEND"],
        },
        "execution_model_assumptions": [
            {
                "id": "assume.fixture",
                "callee": "fork",
                "expected_matches": 1,
                "allowed_source_file_suffixes": ["src/fork.c"],
                "rationale": "fixture execution-model contract",
            }
        ],
        "runtime_hooks": [],
        "generated_support": {
            "copies": [
                {
                    "id": "copy",
                    "source_root": "project",
                    "source": "runtime.c",
                    "target": "src/runtime.c",
                }
            ],
            "edits": [
                {
                    "id": "edit",
                    "path": "src/header.h",
                    "mode": "insert_after",
                    "match": "#define HEADER_H",
                    "content": "\n#define GENERATED 1",
                    "expected_matches": 1,
                    "reject_if_present": "#define GENERATED 1",
                }
            ],
        },
        "test_adaptations": [],
    }


class PostgreSQLAdapterTests(unittest.TestCase):
    def test_project_adapter_claims_only_postgresql_19(self) -> None:
        document = postgresql_adapter.load_adapter(
            PROJECT_ROOT / "manifests/postgresql/adapter.json"
        )
        self.assertEqual(document["id"], "postgresql-19-threaded-runtime")
        self.assertEqual(document["supported_postgresql_majors"], [19])

    def test_project_adapter_makes_posix_semaphore_waits_interruptible(self) -> None:
        document = postgresql_adapter.load_adapter(
            PROJECT_ROOT / "manifests/postgresql/adapter.json"
        )
        edits = [
            edit
            for edit in document["generated_support"]["edits"]
            if edit["path"] == "src/backend/port/posix_sema.c"
        ]
        upstream = (
            PROJECT_ROOT / "postgres/src/backend/port/posix_sema.c"
        ).read_text(encoding="utf-8")
        generated = postgresql_adapter.apply_non_overlapping_edits(edits, upstream)

        self.assertIn("#include <time.h>", generated)
        self.assertIn("if (postgamma_in_backend_thread())", generated)
        self.assertIn("sem_timedwait(PG_SEM_REF(sema), &deadline)", generated)
        self.assertIn("postgamma_dispatch_pending_signals();", generated)
        self.assertIn("errStatus = sem_wait(PG_SEM_REF(sema));", generated)

    def test_project_adapter_exercises_semaphore_wait_during_crash(self) -> None:
        document = postgresql_adapter.load_adapter(
            PROJECT_ROOT / "manifests/postgresql/adapter.json"
        )
        edits = [
            edit
            for edit in document["test_adaptations"]
            if edit["path"] == "src/test/recovery/t/013_crash_restart.pl"
        ]
        upstream = (
            PROJECT_ROOT / "postgres/src/test/recovery/t/013_crash_restart.pl"
        ).read_text(encoding="utf-8")
        generated = postgresql_adapter.apply_non_overlapping_edits(edits, upstream)

        self.assertIn("max_prepared_transactions = 10", generated)
        self.assertIn("PREPARE TRANSACTION 'postgamma_semaphore_blocker'", generated)
        self.assertIn(
            "monitor is blocked in the relation-lock semaphore wait", generated
        )
        self.assertIn("ROLLBACK PREPARED 'postgamma_semaphore_blocker'", generated)

    def test_rejects_unknown_fields_and_malformed_runtime_hooks(self) -> None:
        document = adapter_document()
        document["typo"] = True
        with self.assertRaisesRegex(postgresql_adapter.AdapterError, "unknown field"):
            postgresql_adapter.validate_adapter(document)

        document = adapter_document()
        document["runtime_hooks"] = [
            {
                "id": "hook",
                "enclosing_function": "main",
                "anchor_callee": "entry",
                "position": "beside",
                "code": "run();",
                "expected_matches": 1,
            }
        ]
        with self.assertRaisesRegex(postgresql_adapter.AdapterError, "position"):
            postgresql_adapter.validate_adapter(document)

    def test_validates_major_and_applies_declarative_support(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            tree = root / "tree"
            runtime = root / "generated-runtime"
            (tree / "src").mkdir(parents=True)
            runtime.mkdir()
            (tree / "configure.ac").write_text(
                "AC_INIT([PostgreSQL], [20devel], [bugs@example.test])\n",
                encoding="utf-8",
            )
            (tree / ".postgamma-replacements.json").write_text("{}\n", encoding="utf-8")
            (tree / "src/header.h").write_text("#define HEADER_H\n", encoding="utf-8")
            (root / "runtime.c").write_text("int runtime;\n", encoding="utf-8")
            adapter = root / "adapter.json"
            adapter.write_text(json.dumps(adapter_document()), encoding="utf-8")

            metadata = install_generated_support.install_support(
                root, runtime, tree, adapter
            )
            self.assertEqual(metadata["adapter"]["postgresql_major"], 20)
            self.assertEqual(metadata["adapter"]["support_profile"], "validation")
            self.assertEqual(
                (tree / "src/header.h").read_text(encoding="utf-8"),
                "#define HEADER_H\n#define GENERATED 1\n",
            )
            self.assertEqual(
                (tree / "src/runtime.c").read_text(encoding="utf-8"),
                "int runtime;\n",
            )

    def test_fails_closed_on_major_or_anchor_drift(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "src").mkdir()
            (root / "configure.ac").write_text(
                "AC_INIT([PostgreSQL], [20devel], [bugs@example.test])\n",
                encoding="utf-8",
            )
            (root / "src/header.h").write_text("#define DIFFERENT\n", encoding="utf-8")
            document = postgresql_adapter.validate_adapter(adapter_document(19))
            self.assertEqual(
                postgresql_adapter.inspect_source_compatibility(document, root),
                (20, False),
            )
            with self.assertRaisesRegex(postgresql_adapter.AdapterError, "source is major 20"):
                postgresql_adapter.validate_source_compatibility(document, root)
            document = postgresql_adapter.validate_adapter(adapter_document(20))
            with self.assertRaisesRegex(postgresql_adapter.AdapterError, "found 0"):
                postgresql_adapter.validate_generated_support_anchors(document, root)

    def test_reports_all_anchor_drift_and_keeps_test_adaptations_separate(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "src").mkdir()
            (root / "src/header.h").write_text("different\n", encoding="utf-8")
            (root / "src/test.pl").write_text("upstream test\n", encoding="utf-8")
            document = adapter_document()
            document["generated_support"]["edits"].append(
                {
                    "id": "second-product-edit",
                    "path": "src/header.h",
                    "mode": "replace",
                    "match": "another missing anchor",
                    "content": "replacement",
                    "expected_matches": 1,
                }
            )
            document["test_adaptations"] = [
                {
                    "id": "test-edit",
                    "path": "src/test.pl",
                    "mode": "replace",
                    "match": "missing test anchor",
                    "content": "adapted test",
                    "expected_matches": 1,
                }
            ]
            validated = postgresql_adapter.validate_adapter(document)
            records = postgresql_adapter.inspect_generated_support_anchors(
                validated, root
            )
            self.assertEqual(
                [record["id"] for record in records if record["status"] != "ok"],
                ["edit", "second-product-edit", "test-edit"],
            )
            self.assertEqual(records[-1]["class"], "test")
            with self.assertRaises(postgresql_adapter.AdapterError) as context:
                postgresql_adapter.validate_generated_support_anchors(validated, root)
            message = str(context.exception)
            self.assertIn("edit [product]", message)
            self.assertIn("second-product-edit [product]", message)
            self.assertIn("test-edit [test]", message)
            product_records = postgresql_adapter.inspect_generated_support_anchors(
                validated, root, "product"
            )
            self.assertNotIn("test-edit", [record["id"] for record in product_records])

    def test_replace_between_uses_short_ordered_boundaries(self) -> None:
        edit = {
            "id": "range",
            "path": "src/test.pl",
            "mode": "replace_between",
            "start": "BEGIN",
            "end": "END",
            "content": "replacement",
            "expected_matches": 1,
        }
        text = "prefix BEGIN\nunstable body\nEND suffix"
        inspection = postgresql_adapter.inspect_edit_anchor(edit, text)
        self.assertEqual(inspection["status"], "ok")
        self.assertEqual(inspection["matched_region_length"], len("BEGIN\nunstable body\nEND"))
        self.assertEqual(len(inspection["matched_region_sha256"]), 64)
        self.assertEqual(
            postgresql_adapter.apply_edit(edit, text),
            "prefix replacement suffix",
        )

    def test_restricts_replace_between_to_validation_adaptations(self) -> None:
        document = adapter_document()
        document["generated_support"]["edits"][0] = {
            "id": "range",
            "path": "src/header.h",
            "mode": "replace_between",
            "start": "BEGIN",
            "end": "END",
            "content": "replacement",
            "expected_matches": 1,
        }
        with self.assertRaisesRegex(
            postgresql_adapter.AdapterError, "validation-only"
        ):
            postgresql_adapter.validate_adapter(document)

    def test_detects_edits_that_overlap_on_immutable_source(self) -> None:
        document = adapter_document()
        document["generated_support"]["edits"].append(
            {
                "id": "same-position",
                "path": "src/header.h",
                "mode": "insert_after",
                "match": "#define HEADER_H",
                "content": "\n#define SECOND 1",
                "expected_matches": 1,
            }
        )
        validated = postgresql_adapter.validate_adapter(document)
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "src").mkdir()
            (root / "src/header.h").write_text(
                "#define HEADER_H\n", encoding="utf-8"
            )
            records = postgresql_adapter.inspect_generated_support_anchors(
                validated, root, "product"
            )
        self.assertEqual(
            [record["status"] for record in records],
            ["overlapping_edit", "overlapping_edit"],
        )
        with self.assertRaisesRegex(
            postgresql_adapter.AdapterError, "overlapping generated support edits"
        ):
            postgresql_adapter.apply_non_overlapping_edits(
                [entry for _kind, entry in postgresql_adapter.generated_support_edits(
                    validated, "product"
                )],
                "#define HEADER_H\n",
            )

    def test_candidate_probe_does_not_expand_product_major_contract(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            tree = root / "tree"
            runtime = root / "generated-runtime"
            (tree / "src").mkdir(parents=True)
            runtime.mkdir()
            (tree / "configure.ac").write_text(
                "AC_INIT([PostgreSQL], [21devel], [bugs@example.test])\n",
                encoding="utf-8",
            )
            (tree / ".postgamma-replacements.json").write_text(
                "{}\n", encoding="utf-8"
            )
            (tree / "src/header.h").write_text(
                "#define HEADER_H\n", encoding="utf-8"
            )
            (root / "runtime.c").write_text("int runtime;\n", encoding="utf-8")
            adapter = root / "adapter.json"
            adapter.write_text(json.dumps(adapter_document(20)), encoding="utf-8")

            with self.assertRaisesRegex(
                postgresql_adapter.AdapterError, "source is major 21"
            ):
                install_generated_support.install_support(
                    root, runtime, tree, adapter, "product"
                )
            metadata = install_generated_support.install_support(
                root, runtime, tree, adapter, "product", candidate_probe=True
            )
            self.assertEqual(metadata["adapter"]["postgresql_major"], 21)
            self.assertEqual(
                postgresql_adapter.load_adapter(adapter)[
                    "supported_postgresql_majors"
                ],
                [20],
            )

    def test_exposes_versioned_fact_source_declarations(self) -> None:
        document = postgresql_adapter.validate_adapter(adapter_document())
        self.assertEqual(
            postgresql_adapter.fact_source(document, "guc_catalog"),
            {"path": "src/gucs.dat", "parser": "fixture.gucs.v1"},
        )

    def test_materialization_profile_controls_test_adaptations(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            runtime = root / "generated-runtime"
            runtime.mkdir()
            (root / "runtime.c").write_text("int runtime;\n", encoding="utf-8")
            document = adapter_document()
            document["test_adaptations"] = [
                {
                    "id": "test-edit",
                    "path": "src/test.pl",
                    "mode": "replace",
                    "match": "upstream test",
                    "content": "adapted test",
                    "expected_matches": 1,
                }
            ]
            adapter = root / "adapter.json"
            adapter.write_text(json.dumps(document), encoding="utf-8")

            for profile in ("product", "validation"):
                tree = root / profile
                (tree / "src").mkdir(parents=True)
                (tree / "configure.ac").write_text(
                    "AC_INIT([PostgreSQL], [20devel], [bugs@example.test])\n",
                    encoding="utf-8",
                )
                (tree / ".postgamma-replacements.json").write_text(
                    "{}\n", encoding="utf-8"
                )
                (tree / "src/header.h").write_text(
                    "#define HEADER_H\n", encoding="utf-8"
                )
                (tree / "src/test.pl").write_text(
                    "upstream test\n", encoding="utf-8"
                )
                install_generated_support.install_support(
                    root, runtime, tree, adapter, profile
                )

            self.assertEqual(
                (root / "product/src/test.pl").read_text(encoding="utf-8"),
                "upstream test\n",
            )
            self.assertEqual(
                (root / "validation/src/test.pl").read_text(encoding="utf-8"),
                "adapted test\n",
            )


class BackendStateReviewRuleTests(unittest.TestCase):
    @staticmethod
    def rules() -> dict[str, object]:
        return {
            "schema_version": 1,
            "kind": check_backend_state_policy.REVIEW_RULES_KIND,
            "default_decision": {"owner": "role", "rationale": "default role"},
            "rules": [
                {
                    "id": "external:special",
                    "owner": "session",
                    "rationale": "reviewed session state",
                    "availability": "conditional",
                    "condition": "USE_SPECIAL",
                }
            ],
        }

    def test_requires_every_non_default_and_conditional_decision_in_rules(self) -> None:
        policy = {
            "schema_version": 1,
            "kind": check_backend_state_policy.POLICY_KIND,
            "decisions": [
                {"id": "external:normal", "owner": "role", "rationale": "default"},
                {
                    "id": "external:special",
                    "owner": "session",
                    "rationale": "reviewed session state",
                    "availability": "conditional",
                    "condition": "USE_SPECIAL",
                },
            ],
        }
        report = check_backend_state_policy.validate_review_rule_alignment(
            policy, self.rules()
        )
        self.assertEqual(report["explicit_owner_rule_count"], 1)
        self.assertEqual(report["conditional_rule_count"], 1)
        missing_rules = self.rules()
        missing_rules["rules"] = []
        with self.assertRaisesRegex(
            check_backend_state_policy.BackendStatePolicyError,
            "non-default ownership missing review rule",
        ):
            check_backend_state_policy.validate_review_rule_alignment(
                policy, missing_rules
            )

    def test_policy_proposal_consumes_rules_instead_of_symbol_constants(self) -> None:
        candidates = []
        for name in ("normal", "special"):
            candidates.append(
                {
                    "id": f"external:{name}",
                    "name": name,
                    "usr": f"c:@{name}",
                }
            )
        catalog = {
            "schema_version": 1,
            "kind": check_backend_state_policy.CATALOG_KIND,
            "candidates": candidates,
            "uses": [],
            "summary": {"candidate_count": 2},
        }
        inventory = {"schema_version": 1, "mode": "scan", "declarations": []}
        oracle = {
            "schema_version": 1,
            "kind": propose_backend_state_policy.ORACLE_KIND,
            "matches": [],
        }
        result = propose_backend_state_policy.propose(
            catalog, inventory, oracle, self.rules()
        )
        decisions = {entry["id"]: entry for entry in result["decisions"]}
        self.assertEqual(decisions["external:normal"]["owner"], "role")
        self.assertEqual(decisions["external:special"]["owner"], "session")
        self.assertEqual(
            decisions["external:special"]["condition"], "USE_SPECIAL"
        )

    def test_policy_proposal_does_not_require_a_historical_oracle(self) -> None:
        catalog = {
            "schema_version": 1,
            "kind": check_backend_state_policy.CATALOG_KIND,
            "candidates": [
                {"id": "external:new_state", "name": "new_state", "usr": "c:@new_state"}
            ],
            "uses": [],
            "summary": {"candidate_count": 1},
        }
        inventory = {"schema_version": 1, "mode": "scan", "declarations": []}
        result = propose_backend_state_policy.propose(
            catalog, inventory, None, self.rules()
        )
        self.assertEqual(result["decisions"][0]["owner"], "role")


if __name__ == "__main__":
    unittest.main()
