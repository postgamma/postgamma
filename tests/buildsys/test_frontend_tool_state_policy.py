"""Tests for frontend/tool ownership and bootstrap slice alignment."""

from __future__ import annotations

import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(PROJECT_ROOT / "buildsys"))

import apply_replacements  # noqa: E402
import check_frontend_tool_state_policy as checker  # noqa: E402
import generate_tool_state_plan  # noqa: E402
import generate_tool_state_runtime  # noqa: E402
import reconcile_context_state_plans  # noqa: E402


class FrontendToolStatePolicyTests(unittest.TestCase):
    @staticmethod
    def catalog() -> dict[str, object]:
        candidates = [
            {
                "id": "external:once_state",
                "name": "once_state",
                "usr": "c:@once_state",
            },
            {
                "id": "internal:tool.c:tool.c::buffer",
                "name": "buffer",
                "usr": "c:tool.c@buffer",
            },
            {
                "id": "internal:signal.c:signal.c::handlers",
                "name": "handlers",
                "usr": "c:signal.c@handlers",
            },
        ]
        return {
            "schema_version": 1,
            "kind": "postgamma.backend-mutable-state-catalog",
            "candidates": candidates,
            "uses": [
                {
                    "id": "external:once_state",
                    "use_kind": "read",
                    "in_static_initializer": False,
                },
                {
                    "id": "internal:tool.c:tool.c::buffer",
                    "use_kind": "write",
                    "in_static_initializer": False,
                },
            ],
            "summary": {"candidate_count": len(candidates)},
        }

    @staticmethod
    def policy() -> dict[str, object]:
        return {
            "schema_version": 1,
            "kind": checker.POLICY_KIND,
            "policy": "fixture",
            "decisions": [
                {
                    "id": "external:once_state",
                    "owner": "library_synchronized",
                    "rationale": "initialized once",
                    "synchronization": "pthread_once before publication",
                },
                {
                    "id": "internal:signal.c:signal.c::handlers",
                    "owner": "forbidden",
                    "rationale": "would mutate host signal state",
                    "replacement": "host-neutral callback provider",
                },
                {
                    "id": "internal:tool.c:tool.c::buffer",
                    "owner": "tool",
                    "rationale": "one copy per tool invocation",
                },
            ],
        }

    def test_requires_exact_reviewed_decisions(self) -> None:
        report = checker.validate_alignment(self.catalog(), self.policy())
        self.assertEqual(report["candidate_count"], 3)
        self.assertEqual(report["ownership_counts"]["tool"], 1)
        self.assertEqual(report["ownership_counts"]["forbidden"], 1)

    def test_explicit_bzero_fallback_is_conditional_library_state(self) -> None:
        identifier = (
            "internal:src/port/explicit_bzero.c:"
            "src/port/explicit_bzero.c::bzero_p"
        )
        policy = checker.load_json(
            PROJECT_ROOT / "manifests/ownership/frontend-tool-state.json",
            checker.FrontendToolStatePolicyError,
        )
        decision = checker.policy_decisions(policy)[identifier]
        self.assertEqual(
            decision,
            {
                "id": identifier,
                "owner": "library_immutable",
                "rationale": (
                    "volatile call-through pointer is initialized once to bzero2 "
                    "and never assigned; volatility only prevents dead-store "
                    "elimination"
                ),
                "availability": "conditional",
                "condition": (
                    "!HAVE_MEMSET_EXPLICIT && !HAVE_EXPLICIT_MEMSET && "
                    "!HAVE_DECL_MEMSET_S && !WIN32"
                ),
            },
        )

    def test_rejects_missing_synchronization_contract(self) -> None:
        policy = self.policy()
        del policy["decisions"][0]["synchronization"]
        with self.assertRaisesRegex(
            checker.FrontendToolStatePolicyError, "synchronization contract"
        ):
            checker.validate_alignment(self.catalog(), policy)

    def test_rejects_runtime_writes_to_immutable_state(self) -> None:
        policy = self.policy()
        policy["decisions"][2]["owner"] = "library_immutable"
        with self.assertRaisesRegex(
            checker.FrontendToolStatePolicyError, "runtime write"
        ):
            checker.validate_alignment(self.catalog(), policy)

    def test_forbidden_state_blocks_a_reachable_slice(self) -> None:
        reachability = {
            "schema_version": 1,
            "kind": checker.REACHABILITY_KIND,
            "slices": [
                {
                    "id": "private_libpq",
                    "candidate_ids": ["internal:signal.c:signal.c::handlers"],
                    "provenance": "fixture link closure",
                }
            ],
        }
        with self.assertRaisesRegex(
            checker.FrontendToolStatePolicyError, "reaches forbidden state"
        ):
            checker.validate_alignment(self.catalog(), self.policy(), reachability)

    def test_rejects_duplicate_json_keys(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "duplicate.json"
            path.write_text('{"schema_version": 1, "schema_version": 1}\n')
            with self.assertRaisesRegex(
                checker.FrontendToolStatePolicyError, "duplicate JSON key"
            ):
                checker.load_json(path, checker.FrontendToolStatePolicyError)

    def test_generates_an_aligned_invocation_owned_layout(self) -> None:
        catalog = self.catalog()
        for candidate in catalog["candidates"]:
            candidate.update(
                {
                    "canonical_type": "int",
                    "size": 4,
                    "alignment": 4,
                    "has_initializer": candidate["name"] == "buffer",
                }
            )
        alignment = checker.validate_alignment(catalog, self.policy())
        slots, byte_count, maximum_alignment = (
            generate_tool_state_runtime.compile_slots(
                catalog, self.policy(), alignment
            )
        )
        self.assertEqual(len(slots), 1)
        self.assertEqual(byte_count, 4)
        self.assertEqual(maximum_alignment, 4)
        self.assertTrue(slots[0].copy_template)
        layout = generate_tool_state_runtime.emit_layout(
            slots, byte_count, maximum_alignment
        )
        self.assertIn("POSTGAMMA_TOOL_STATE_SLOT_COUNT = 1", layout)
        self.assertNotIn("postgamma_tool_state_create(", layout)

    def test_relocation_bridge_freezes_ast_initializer_match_count(self) -> None:
        owner_id = "internal:tool.c:tool.c::owner"
        target_id = "internal:tool.c:tool.c::target"
        catalog = {
            "schema_version": 1,
            "kind": "postgamma.backend-mutable-state-catalog",
            "candidates": [
                {
                    "id": owner_id,
                    "name": "owner",
                    "usr": "c:tool.c@owner",
                    "canonical_type": "void *[2]",
                    "size": 16,
                    "alignment": 8,
                    "has_initializer": True,
                },
                {
                    "id": target_id,
                    "name": "target",
                    "usr": "c:tool.c@target",
                    "canonical_type": "int[2]",
                    "size": 8,
                    "alignment": 4,
                    "has_initializer": True,
                },
            ],
            "uses": [
                {
                    "id": target_id,
                    "path": "tool.c",
                    "offset": offset,
                    "length": 6,
                    "in_static_initializer": True,
                    "static_initializer_owner_id": owner_id,
                }
                for offset in (40, 72)
            ],
            "summary": {"candidate_count": 2},
        }
        policy = {
            "schema_version": 1,
            "kind": checker.POLICY_KIND,
            "decisions": [
                {
                    "id": owner_id,
                    "owner": "tool",
                    "rationale": "fixture owner",
                },
                {
                    "id": target_id,
                    "owner": "tool",
                    "rationale": "fixture target",
                },
            ]
        }
        alignment = {
            "relocations": [{"owner_id": owner_id, "target_id": target_id}]
        }
        slots, _bytes, _alignment = generate_tool_state_runtime.compile_slots(
            catalog, policy, alignment
        )
        runtime_by_id = {
            slot.identifier: {
                "id": slot.identifier,
                "name": slot.name,
                "enum": slot.enum_name,
                "relocations": slot.relocations,
                "relocation_counts": slot.relocation_counts,
            }
            for slot in slots
        }
        owner_runtime = runtime_by_id[owner_id]
        self.assertEqual(owner_runtime["relocation_counts"], {target_id: 2})
        bridge = generate_tool_state_plan.relocation_bridge(
            {"id": owner_id, "name": "owner"},
            owner_runtime,
            runtime_by_id,
        )
        self.assertIn("&(target), 2", bridge)

    def test_function_static_relocation_is_inlined_at_use(self) -> None:
        owner_id = "function_static:tool.c:tool.c:run:owner"
        target_id = "internal:tool.c:tool.c::target"
        source_text = (
            "#include <stddef.h>\n"
            "#ifdef POSTGAMMA_FRONTEND_TOOL\n"
            '#include "postgamma/tool_state_runtime.h"\n'
            "#else\n"
            "#define POSTGAMMA_TOOL_STATE_RELOCATED_INLINE_VALUE("
            "original, slot, targets, target_count) (original)\n"
            "#endif\n"
            "static int target;\n"
            "void consume(int **);\n"
            "void run(void)\n"
            "{\n"
            "    static int *owner[] = {&target};\n"
            "    consume(owner);\n"
            "}\n"
        )
        target_offset = source_text.index("target", source_text.index("{&target"))
        owner_offset = source_text.rindex("owner")
        catalog = {
            "schema_version": 1,
            "kind": "postgamma.backend-mutable-state-catalog",
            "candidates": [
                {
                    "id": owner_id,
                    "name": "owner",
                    "usr": "c:tool.c@F@run@owner",
                    "canonical_type": "int *[1]",
                    "size": 8,
                    "alignment": 8,
                    "has_initializer": True,
                    "function_static": True,
                    "file_scope": False,
                    "definition_path": "tool.c",
                },
                {
                    "id": target_id,
                    "name": "target",
                    "usr": "c:tool.c@target",
                    "canonical_type": "int",
                    "size": 4,
                    "alignment": 4,
                    "has_initializer": False,
                    "function_static": False,
                    "file_scope": True,
                    "definition_path": "tool.c",
                },
            ],
            "uses": [
                {
                    "id": target_id,
                    "path": "tool.c",
                    "offset": target_offset,
                    "length": len("target"),
                    "in_static_initializer": True,
                    "static_initializer_owner_id": owner_id,
                },
                {
                    "id": owner_id,
                    "path": "tool.c",
                    "offset": owner_offset,
                    "length": len("owner"),
                    "in_static_initializer": False,
                    "use_kind": "read",
                },
            ],
            "summary": {"candidate_count": 2},
        }
        policy = {
            "schema_version": 1,
            "kind": checker.POLICY_KIND,
            "policy": "fixture",
            "decisions": [
                {
                    "id": owner_id,
                    "owner": "tool",
                    "rationale": "fixture local owner",
                },
                {
                    "id": target_id,
                    "owner": "tool",
                    "rationale": "fixture relocation target",
                },
            ],
        }
        alignment = {
            "relocations": [{"owner_id": owner_id, "target_id": target_id}]
        }
        slots, byte_count, maximum_alignment = (
            generate_tool_state_runtime.compile_slots(catalog, policy, alignment)
        )
        runtime = {
            "schema_version": 1,
            "kind": generate_tool_state_runtime.RUNTIME_KIND,
            "slots": [
                {
                    "id": slot.identifier,
                    "name": slot.name,
                    "enum": slot.enum_name,
                    "function_static": slot.function_static,
                    "relocations": slot.relocations,
                    "relocation_counts": slot.relocation_counts,
                }
                for slot in slots
            ],
        }
        layout = generate_tool_state_runtime.emit_layout(
            slots, byte_count, maximum_alignment
        )
        self.assertNotIn(
            generate_tool_state_runtime.relocation_bridge_name(owner_id), layout
        )

        with tempfile.TemporaryDirectory() as temporary:
            source_root = Path(temporary)
            (source_root / "tool.c").write_text(source_text, encoding="ascii")
            plan = generate_tool_state_plan.compile_plan(
                catalog, policy, runtime, source_root
            )
            self.assertEqual(plan["summary"]["relocation_bridge_count"], 0)
            self.assertEqual(plan["summary"]["inline_relocation_count"], 1)
            replacements = plan["files"][0]["replacements"]
            self.assertEqual(len(replacements), 1)
            self.assertEqual(replacements[0]["kind"], "tool_state")
            self.assertIn(
                "POSTGAMMA_TOOL_STATE_RELOCATED_INLINE_VALUE",
                replacements[0]["replacement"],
            )
            self.assertIn("&(target), 1", replacements[0]["replacement"])
            transformed, count = apply_replacements.apply_bytes(
                source_text.encode("ascii"), plan["files"][0]
            )
            self.assertEqual(count, 1)

            compiler = shutil.which("cc")
            if compiler is not None:
                include = source_root / "postgamma"
                include.mkdir()
                (include / "tool_state_layout.h").write_text(
                    layout, encoding="ascii"
                )
                for frontend_tool in (False, True):
                    with self.subTest(frontend_tool=frontend_tool):
                        generated = source_root / (
                            "generated-tool.c"
                            if frontend_tool
                            else "generated-ordinary.c"
                        )
                        generated.write_bytes(transformed)
                        command = [
                            compiler,
                            "-std=gnu11",
                            "-Wall",
                            "-Wextra",
                            "-Werror",
                            f"-I{source_root}",
                            f"-I{PROJECT_ROOT / 'runtime/include'}",
                            "-fsyntax-only",
                            str(generated),
                        ]
                        if frontend_tool:
                            command.insert(1, "-DPOSTGAMMA_FRONTEND_TOOL")
                        result = subprocess.run(
                            command,
                            text=True,
                            stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT,
                            check=False,
                        )
                        self.assertEqual(result.returncode, 0, result.stdout)

    def test_reconciles_a_shared_source_token_by_compile_domain(self) -> None:
        digest = "0123456789ABCDEF"
        base = {
            "offset": 10,
            "length": 6,
            "original": "buffer",
            "rule_id": "internal:tool.c:tool.c::buffer",
            "use_kind": "write",
        }

        def plan(kind: str, replacement: str) -> dict[str, object]:
            return {
                "schema_version": 1,
                "mode": "plan",
                "kind": kind,
                "files": [
                    {
                        "path": "tool.c",
                        "sha256": "same",
                        "replacements": [{**base, "replacement": replacement}],
                    }
                ],
            }

        result = reconcile_context_state_plans.reconcile(
            plan(
                "postgamma.backend-state-plan",
                "POSTGAMMA_BACKEND_STATE_VALUE("
                f"POSTGAMMA_BACKEND_STATE_SLOT_{digest}, buffer)",
            ),
            plan(
                "postgamma.tool-state-plan",
                "POSTGAMMA_TOOL_STATE_VALUE("
                f"POSTGAMMA_TOOL_STATE_SLOT_{digest}, buffer)",
            ),
        )
        replacement = result["files"][0]["replacements"][0]
        self.assertEqual(
            replacement["replacement"],
            f"POSTGAMMA_CONTEXT_STATE_VALUE({digest}, buffer)",
        )
        self.assertEqual(result["summary"]["shared_domain_replacement_count"], 1)

    def test_frontend_phase_hook_is_scoped_and_fails_closed_on_drift(self) -> None:
        catalog = {
            "schema_version": 1,
            "kind": "postgamma.backend-mutable-state-catalog",
            "candidates": [],
            "uses": [],
            "summary": {"candidate_count": 0},
        }
        policy = {
            "schema_version": 1,
            "kind": checker.POLICY_KIND,
            "policy": "fixture",
            "decisions": [],
        }
        runtime = {
            "schema_version": 1,
            "kind": generate_tool_state_runtime.RUNTIME_KIND,
            "slots": [],
        }
        adapter = {
            "frontend_tool_hooks": [
                {
                    "id": "fixture-check-phase",
                    "source_file": "src/bin/initdb/initdb.c",
                    "enclosing_function": "test_specific_config_settings",
                    "expected_definitions": 1,
                    "anchor": "status = system(cmd.data);",
                    "replacement": (
                        "POSTGAMMA_INITDB_PHASE_SELECT(phase, 0, false);\n"
                        "\tstatus = system(cmd.data);"
                    ),
                    "expected_matches": 1,
                }
            ]
        }
        with tempfile.TemporaryDirectory() as temporary:
            source_root = Path(temporary)
            source = source_root / "src/bin/initdb/initdb.c"
            source.parent.mkdir(parents=True)
            source.write_text(
                "static void\n"
                "test_specific_config_settings(void)\n"
                "{\n"
                "\tint status;\n"
                "\tstatus = system(cmd.data);\n"
                "}\n",
                encoding="utf-8",
            )
            plan = generate_tool_state_plan.compile_plan(
                catalog, policy, runtime, source_root, adapter
            )
            replacements = plan["files"][0]["replacements"]
            self.assertEqual(len(replacements), 1)
            self.assertEqual(replacements[0]["kind"], "frontend_tool_phase")
            self.assertEqual(
                plan["summary"]["frontend_tool_phase_hook_count"], 1
            )

            source.write_text(
                source.read_text(encoding="utf-8").replace(
                    "status = system(cmd.data);", "status = run_check();"
                ),
                encoding="utf-8",
            )
            with self.assertRaisesRegex(
                checker.FrontendToolStatePolicyError,
                "expected 1 match",
            ):
                generate_tool_state_plan.compile_plan(
                    catalog, policy, runtime, source_root, adapter
                )


if __name__ == "__main__":
    unittest.main()
