from __future__ import annotations

import json
import sys
import tempfile
import unittest
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(PROJECT_ROOT / "buildsys"))

import apply_replacements  # noqa: E402
import cc_capture  # noqa: E402
import check_backend_execution_policy  # noqa: E402
import check_backend_state_policy  # noqa: E402
import check_layout  # noqa: E402
import check_portability  # noqa: E402
import check_guc_policy  # noqa: E402
import configure_postgres  # noqa: E402
import generate_backend_execution_catalog  # noqa: E402
import generate_backend_execution_facts  # noqa: E402
import generate_backend_inheritance  # noqa: E402
import generate_backend_state_plan  # noqa: E402
import generate_backend_state_runtime  # noqa: E402
import generate_guc_catalog  # noqa: E402
import generate_guc_transform_manifest  # noqa: E402
import generate_guc_runtime  # noqa: E402
import install_generated_artifacts  # noqa: E402
import list_compile_sources  # noqa: E402
import merge_compile_db  # noqa: E402
import merge_replacement_plans  # noqa: E402
import reset_directory  # noqa: E402
import toolchain  # noqa: E402


class ResetDirectoryTests(unittest.TestCase):
    def test_requires_strict_child(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            self.assertTrue(reset_directory.is_strict_child(root / "one", root))
            self.assertFalse(reset_directory.is_strict_child(root, root))
            self.assertFalse(reset_directory.is_strict_child(root.parent, root))


class CaptureTests(unittest.TestCase):
    def test_parses_wrapper_only_compiler_arguments(self) -> None:
        self.assertEqual(
            cc_capture.compiler_arguments_from_environment(
                {
                    "POSTGAMMA_REAL_CC_ARGS": (
                        "-ffile-prefix-map='/tmp/source tree'=. "
                        "-fmacro-prefix-map=/tmp/source=."
                    )
                }
            ),
            [
                "-ffile-prefix-map=/tmp/source tree=.",
                "-fmacro-prefix-map=/tmp/source=.",
            ],
        )

    def test_finds_only_existing_compile_inputs(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "sample.c"
            source.write_text("int value;\n", encoding="utf-8")
            result = cc_capture.source_arguments(
                ["-Iinclude", "-c", "sample.c", "-o", "sample.o"], root
            )
            self.assertEqual(result, [source.resolve()])
            self.assertEqual(cc_capture.source_arguments(["sample.o", "-o", "app"], root), [])


class ReplacementTests(unittest.TestCase):
    def test_applies_offsets_in_reverse_and_checks_hash(self) -> None:
        import hashlib

        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            source = root / "sample.c"
            original = b"int one = one + two;\n"
            source.write_bytes(original)
            entry = {
                "path": "sample.c",
                "sha256": hashlib.sha256(original).hexdigest(),
                "replacements": [
                    {"offset": 10, "length": 3, "original": "one", "replacement": "FIRST"},
                    {"offset": 16, "length": 3, "original": "two", "replacement": "SECOND"},
                ],
            }
            self.assertEqual(apply_replacements.apply_file(source, entry), 2)
            self.assertEqual(source.read_text(encoding="utf-8"), "int one = FIRST + SECOND;\n")

    def test_rejects_parent_path(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            with self.assertRaises(ValueError):
                apply_replacements.safe_target(root, "../outside.c")


class ConfigureTests(unittest.TestCase):
    def test_arguments_are_parsed_without_losing_spaces(self) -> None:
        environment = {
            "COMMON": "--enable-debug --prefix='/tmp/postgamma install'",
            "SPECIFIC": "--without-icu",
        }
        self.assertEqual(
            configure_postgres.arguments_from_environment(
                ("COMMON", "SPECIFIC"), environment
            ),
            ["--enable-debug", "--prefix=/tmp/postgamma install", "--without-icu"],
        )

    def test_changed_arguments_reconfigure_and_unchanged_arguments_do_not(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            source = root / "source"
            build_root = root / "build"
            build = build_root / "reference"
            source.mkdir()
            configure = source / "configure"
            configure.write_text(
                "#!/bin/sh\n"
                "printf '%s\\n' \"$@\" > configure-arguments.txt\n"
                "touch config.status\n",
                encoding="utf-8",
            )
            configure.chmod(0o755)
            environment = {"PATH": "/usr/bin:/bin"}
            dependent = build_root / "dependent"
            dependent.mkdir(parents=True)
            dependent_marker = dependent / "stale"
            dependent_marker.touch()

            self.assertTrue(
                configure_postgres.ensure_configured(
                    source,
                    build,
                    build_root,
                    ["--enable-debug"],
                    environment,
                    [dependent],
                )
            )
            self.assertFalse(dependent.exists())
            dependent.mkdir()
            dependent_marker = dependent / "preserve-on-cache-hit"
            dependent_marker.touch()
            marker = build / "preserve-on-cache-hit"
            marker.touch()
            self.assertFalse(
                configure_postgres.ensure_configured(
                    source,
                    build,
                    build_root,
                    ["--enable-debug"],
                    environment,
                    [dependent],
                )
            )
            self.assertTrue(marker.exists())
            self.assertTrue(dependent_marker.exists())

            self.assertTrue(
                configure_postgres.ensure_configured(
                    source,
                    build,
                    build_root,
                    ["--without-icu"],
                    environment,
                    [dependent],
                )
            )
            self.assertFalse(marker.exists())
            self.assertFalse(dependent.exists())
            self.assertEqual(
                (build / "configure-arguments.txt").read_text(encoding="utf-8"),
                "--without-icu\n",
            )


class PortabilityTests(unittest.TestCase):
    def test_scans_untracked_project_files_and_excludes_generated_trees(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "source").mkdir()
            (root / "source/untracked.txt").write_text("source\n", encoding="utf-8")
            for excluded in (".deps", ".git", "build", "postgres"):
                (root / excluded).mkdir()
                (root / excluded / "ignored.txt").write_text(
                    "generated\n", encoding="utf-8"
                )
            paths = {
                path.relative_to(root).as_posix()
                for path in check_portability.project_files(root)
            }
            self.assertEqual(paths, {"source/untracked.txt"})

    def test_detects_developer_workspace_paths(self) -> None:
        data = b'path = "' + b"/" + b'home/example/codes/project"\n'
        self.assertEqual(check_portability.forbidden_offsets(data, ()), [8])

    def test_allows_system_and_relative_paths(self) -> None:
        data = b"/usr/lib/llvm\n../postgres\n"
        self.assertEqual(check_portability.forbidden_offsets(data, ()), [])

    def test_explicit_workspace_path_requires_a_component_boundary(self) -> None:
        data = b".github/workflows/build.yml\n" + b"/" + b"work/build/result.json\n"
        prefix = check_portability.directory_prefix(Path("/") / "work")
        self.assertEqual(prefix, b"/" + b"work/")
        self.assertEqual(
            check_portability.forbidden_offsets(data, (prefix,)),
            [data.index(b"/" + b"work/build")],
        )

    def test_shallow_workspace_prefix_starts_an_absolute_path_token(self) -> None:
        prefix = b"/" + b"src/"
        relative_suffix = b"$(BUILD_DIR)/src/include\n"
        absolute_path = b'input = "' + prefix + b'generated/input.c"\n'
        url = b"https://src/example\n"
        data = relative_suffix + absolute_path + url
        self.assertEqual(
            check_portability.forbidden_offsets(data, (prefix,)),
            [len(relative_suffix) + absolute_path.index(prefix)],
        )

    def test_generic_top_level_checkout_is_not_a_developer_prefix(self) -> None:
        self.assertEqual(
            check_portability.developer_directory_prefixes(
                (Path("/") / "src", Path("/") / "root")
            ),
            (),
        )
        self.assertEqual(
            check_portability.developer_directory_prefixes(
                (Path("/") / "workspace" / "project",)
            ),
            (b"/" + b"workspace/project/",),
        )


class BackendExecutionCatalogTests(unittest.TestCase):
    def test_parses_nested_macro_arguments(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary) / "proctypelist.h"
            source.write_text(
                'PG_PROCTYPE(B_BACKEND, "backend", '
                'gettext_noop("client, backend"), BackendMain, true)\n'
                'PG_PROCTYPE(B_ALIAS, "backend", '
                'gettext_noop("logical alias"), NULL, false)\n',
                encoding="utf-8",
            )
            executions = generate_backend_execution_catalog.parse_proctypelist(source)
        self.assertEqual([entry["symbol"] for entry in executions], ["B_ALIAS", "B_BACKEND"])
        self.assertEqual(executions[1]["description"], "client, backend")
        self.assertEqual(executions[1]["main_function"], "BackendMain")
        self.assertTrue(executions[1]["shared_memory_access"])

    def test_policy_requires_exact_alignment_and_valid_dispositions(self) -> None:
        catalog = {
            "executions": [
                {"symbol": "B_BACKEND", "main_function": "BackendMain"},
                {"symbol": "B_ALIAS", "main_function": None},
            ]
        }
        policy = {
            "executions": [
                {
                    "symbol": "B_BACKEND",
                    "disposition": "thread",
                    "execution_class": "client",
                    "rationale": "client execution",
                },
                {
                    "symbol": "B_ALIAS",
                    "disposition": "logical_alias",
                    "rationale": "changes type after launch",
                },
            ]
        }
        alignment = check_backend_execution_policy.compile_alignment(catalog, policy)
        self.assertEqual(alignment["summary"]["threaded_count"], 1)
        self.assertEqual(alignment["summary"]["logical_alias_count"], 1)
        facts = generate_backend_execution_facts.emit_facts(
            {
                "schema_version": 1,
                "kind": generate_backend_execution_facts.ALIGNMENT_KIND,
                **alignment,
            }
        )
        self.assertIn(
            "POSTGAMMA_BACKEND_EXECUTION(B_BACKEND, "
            "POSTGAMMA_BACKEND_DISPOSITION_THREAD, "
            "POSTGAMMA_BACKEND_POLICY_CLASS_CLIENT)",
            facts,
        )
        self.assertNotIn("postgamma_backend_launch(", facts)

        policy["executions"].pop()
        with self.assertRaisesRegex(ValueError, "missing decisions: B_ALIAS"):
            check_backend_execution_policy.compile_alignment(catalog, policy)


class BackendInheritanceTests(unittest.TestCase):
    def test_requires_exact_catalog_and_copyable_role_slots(self) -> None:
        catalog = {
            "contract": {
                "function": "save_backend_variables",
                "translation_unit": "src/backend/postmaster/launch_backend.c",
            },
            "states": [
                {
                    "id": "external:shared_header",
                    "name": "shared_header",
                    "canonical_type": "void *",
                },
                {
                    "id": "external:start_time",
                    "name": "start_time",
                    "canonical_type": "long",
                },
            ],
        }
        policy = {
            "guc_transfer": "postgresql_serialize_restore",
            "startup_data_transfer": "copy_bytes",
            "client_socket_transfer": "move_descriptor_ownership",
            "states": [
                {
                    "id": "external:shared_header",
                    "strategy": "borrow_instance",
                    "rationale": "instance shared memory",
                },
                {
                    "id": "external:start_time",
                    "strategy": "copy_value",
                    "rationale": "copied scalar",
                },
            ],
        }
        runtime = {
            "slots": [
                {
                    "id": "external:shared_header",
                    "owner": "role",
                    "relocations": [],
                    "enum": "POSTGAMMA_SLOT_SHARED",
                    "size": 8,
                    "alignment": 8,
                },
                {
                    "id": "external:start_time",
                    "owner": "role",
                    "relocations": [],
                    "enum": "POSTGAMMA_SLOT_TIME",
                    "size": 8,
                    "alignment": 8,
                },
            ]
        }
        alignment = generate_backend_inheritance.compile_alignment(
            catalog, policy, runtime
        )
        self.assertEqual(alignment["summary"]["state_count"], 2)
        facts = generate_backend_inheritance.render_facts(alignment)
        self.assertIn('"external:shared_header"', facts)
        self.assertNotIn("postgamma_backend_state_context_copy_ids(", facts)

        policy["states"].pop()
        with self.assertRaisesRegex(ValueError, "missing decisions: external:start_time"):
            generate_backend_inheritance.compile_alignment(catalog, policy, runtime)

    def test_rejects_relocated_inherited_state(self) -> None:
        catalog = {
            "contract": {"function": "save", "translation_unit": "launch.c"},
            "states": [{"id": "external:value", "name": "value"}],
        }
        policy = {
            "guc_transfer": "postgresql_serialize_restore",
            "startup_data_transfer": "copy_bytes",
            "client_socket_transfer": "move_descriptor_ownership",
            "states": [
                {
                    "id": "external:value",
                    "strategy": "copy_value",
                    "rationale": "value",
                }
            ],
        }
        runtime = {
            "slots": [
                {
                    "id": "external:value",
                    "owner": "role",
                    "relocations": ["external:target"],
                }
            ]
        }
        with self.assertRaisesRegex(ValueError, "must not require pointer relocation"):
            generate_backend_inheritance.compile_alignment(catalog, policy, runtime)


class LayoutTests(unittest.TestCase):
    def test_rejects_non_python_project_scripts(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            buildsys = root / "buildsys"
            buildsys.mkdir()
            (buildsys / "accepted.py").write_text("", encoding="utf-8")
            rejected = buildsys / "legacy.pl"
            rejected.write_text("", encoding="utf-8")
            extensionless = buildsys / "legacy"
            extensionless.write_text("#!/bin/sh\n", encoding="utf-8")
            self.assertEqual(
                check_layout.foreign_scripts(root),
                [Path("buildsys/legacy"), Path("buildsys/legacy.pl")],
            )


class GucCatalogTests(unittest.TestCase):
    def test_parses_supported_perl_data_without_evaluating_it(self) -> None:
        source = Path("guc_parameters.dat")
        data = rb"""[
# leading comment
{ name => 'sample', type => 'string', context => 'PGC_USERSET',
  group => 'CLIENT_CONN_OTHER', variable => 'sample_value',
  boot_val => 'C:\\tmp', short_desc => 'the client\'s value', # inline comment
},
/* an upstream C-style comment between records */
]"""
        entries = generate_guc_catalog.parse_catalog_data(data, source)
        self.assertEqual(len(entries), 1)
        fields, line_number = entries[0]
        self.assertEqual(line_number, 3)
        self.assertEqual(fields["boot_val"], r"C:\tmp")
        self.assertEqual(fields["short_desc"], "the client's value")

    def test_rejects_new_unreviewed_value_syntax(self) -> None:
        with self.assertRaisesRegex(
            generate_guc_catalog.CatalogDataError,
            "expected a single-quoted string",
        ):
            generate_guc_catalog.parse_catalog_data(
                b"[{ name => expression }]", Path("guc_parameters.dat")
            )

    def test_preserves_canonical_catalog_layout(self) -> None:
        self.assertEqual(
            generate_guc_catalog.encode_catalog_json({"items": [1], "empty": {}}),
            "{\n"
            '   "empty" : {},\n'
            '   "items" : [\n'
            "      1\n"
            "   ]\n"
            "}",
        )

    def test_rejects_duplicate_storage_symbols(self) -> None:
        first = {
            "name": "first",
            "variable": "shared",
            "type": "bool",
            "context": "PGC_USERSET",
            "group": "TESTING_OPTIONS",
            "boot_val": "false",
        }
        second = dict(first, name="second")
        with self.assertRaisesRegex(
            generate_guc_catalog.CatalogDataError, "duplicate storage symbol shared"
        ):
            generate_guc_catalog.catalog_document(
                [(first, 1), (second, 2)], b"input", Path("input.dat"), "input.dat"
            )


class GucPolicyTests(unittest.TestCase):
    def test_requires_exact_catalog_alignment(self) -> None:
        policy = {"first": "session", "second": "instance"}
        report = check_guc_policy.validate_alignment({"first", "second"}, policy)
        self.assertEqual(report["missing"], [])
        self.assertEqual(report["stale"], [])
        self.assertEqual(report["ownership_counts"]["session"], 1)

    def test_reports_missing_and_stale_decisions(self) -> None:
        with self.assertRaisesRegex(
            check_guc_policy.PolicyError,
            r"missing ownership decision\(s\): second; stale ownership decision\(s\): old",
        ):
            check_guc_policy.validate_alignment(
                {"first", "second"}, {"first": "session", "old": "instance"}
            )

    def test_rejects_an_owner_the_runtime_does_not_define(self) -> None:
        document = {
            "schema_version": 1,
            "kind": check_guc_policy.POLICY_KIND,
            "parameters": {"work_mem": "probably-session"},
        }
        with self.assertRaisesRegex(check_guc_policy.PolicyError, "invalid owner"):
            check_guc_policy.policy_owners(document)


class GucTransformManifestTests(unittest.TestCase):
    @staticmethod
    def catalog(*parameters: dict[str, str]) -> dict[str, object]:
        return {
            "schema_version": 1,
            "kind": check_guc_policy.CATALOG_KIND,
            "parameter_count": len(parameters),
            "parameters": list(parameters),
        }

    @staticmethod
    def policy(**parameters: str) -> dict[str, object]:
        return {
            "schema_version": 1,
            "kind": check_guc_policy.POLICY_KIND,
            "parameters": parameters,
        }

    @staticmethod
    def hooks() -> dict[str, object]:
        return {
            "schema_version": 1,
            "kind": generate_guc_transform_manifest.HOOK_KIND,
            "hooks": [],
            "assumptions": [],
        }

    @staticmethod
    def domain() -> dict[str, object]:
        return {
            "schema_version": 1,
            "kind": generate_guc_transform_manifest.DOMAIN_KIND,
            "translation_unit_prefixes": ["src/backend/"],
            "translation_unit_files": ["src/test/regress/regress.c"],
            "binding_anchor_file_suffixes": [
                "src/backend/utils/guc_tables.inc.c",
                "src/include/utils/guc_tables.inc.c",
            ],
        }

    @staticmethod
    def controls() -> dict[str, object]:
        return {
            "schema_version": 1,
            "kind": generate_guc_transform_manifest.CONTROL_KIND,
            "symbols": [],
        }

    def test_compiles_catalog_facts_and_human_ownership(self) -> None:
        catalog = self.catalog(
            {
                "name": "work_mem",
                "storage_symbol": "work_mem",
                "value_type": "int",
                "compile_condition": "",
            },
            {
                "name": "bonjour",
                "storage_symbol": "EnableBonjour",
                "value_type": "bool",
                "compile_condition": "defined(USE_BONJOUR)",
            },
        )
        result = generate_guc_transform_manifest.compile_manifest(
            catalog,
            self.policy(work_mem="session", bonjour="instance"),
            self.hooks(),
            self.domain(),
            self.controls(),
        )
        self.assertEqual(
            [symbol["name"] for symbol in result["symbols"]],
            ["EnableBonjour", "work_mem"],
        )
        self.assertEqual(
            result["binding_anchor_file_suffixes"],
            [
                "src/backend/utils/guc_tables.inc.c",
                "src/include/utils/guc_tables.inc.c",
            ],
        )
        self.assertTrue(result["symbols"][0]["optional"])
        self.assertEqual(result["symbols"][1]["slot"], "session")
        self.assertEqual(
            result["symbols"][1]["replacement"],
            "POSTGAMMA_GUC_VALUE(work_mem)",
        )

    def test_rejects_two_gucs_sharing_one_storage_symbol(self) -> None:
        catalog = self.catalog(
            {
                "name": "first",
                "storage_symbol": "same_storage",
                "value_type": "int",
                "compile_condition": "",
            },
            {
                "name": "second",
                "storage_symbol": "same_storage",
                "value_type": "int",
                "compile_condition": "",
            },
        )
        with self.assertRaisesRegex(check_guc_policy.PolicyError, "duplicate storage"):
            generate_guc_transform_manifest.compile_manifest(
                catalog,
                self.policy(first="session", second="session"),
                self.hooks(),
                self.domain(),
                self.controls(),
            )


class GucRuntimeGeneratorTests(unittest.TestCase):
    @staticmethod
    def catalog() -> dict[str, object]:
        parameters = [
            {
                "name": "enable_hashjoin",
                "storage_symbol": "enable_hashjoin",
                "value_type": "bool",
                "compile_condition": "",
            },
            {
                "name": "max_connections",
                "storage_symbol": "MaxConnections",
                "value_type": "int",
                "compile_condition": "HAVE_MAX_CONNECTIONS",
            },
            {
                "name": "work_mem",
                "storage_symbol": "work_mem",
                "value_type": "int",
                "compile_condition": "",
            },
        ]
        return {
            "schema_version": 1,
            "kind": generate_guc_runtime.CATALOG_KIND,
            "parameter_count": len(parameters),
            "parameters": parameters,
        }

    @staticmethod
    def policy() -> dict[str, object]:
        return {
            "schema_version": 1,
            "kind": check_guc_policy.POLICY_KIND,
            "parameters": {
                "enable_hashjoin": "session",
                "max_connections": "instance",
                "work_mem": "session",
            },
        }

    @staticmethod
    def controls() -> dict[str, object]:
        return {
            "schema_version": 1,
            "kind": generate_guc_runtime.CONTROL_KIND,
            "symbols": [
                {
                    "id": "pg.guc.control.configure_names",
                    "name": "ConfigureNames",
                    "slot": "session",
                    "replacement": "POSTGAMMA_GUC_CONTROL_CONFIGURE_NAMES",
                    "runtime_field": "configure_names",
                    "runtime_c_type": "struct config_generic *",
                    "initializer": "configure_names",
                }
            ],
        }

    def test_generates_typed_owner_paths_and_conditional_bridge(self) -> None:
        first = generate_guc_runtime.compile_runtime(
            self.catalog(), self.policy(), self.controls()
        )
        second = generate_guc_runtime.compile_runtime(
            self.catalog(), self.policy(), self.controls()
        )
        self.assertEqual(first, second)
        layout = first["include/postgamma/guc_state_layout.h"]
        descriptors = first["include/postgamma/guc_descriptors.inc"]
        self.assertIn(
            "postgamma_execution_context_require()->session_guc.work_mem", layout
        )
        self.assertIn(
            "postgamma_execution_context_require()->instance->instance_guc.MaxConnections",
            layout,
        )
        self.assertIn("#ifdef HAVE_MAX_CONNECTIONS", descriptors)
        self.assertIn("POSTGAMMA_GUC_SLOT_COUNT = 3", layout)

    def test_generator_emits_guc_facts_without_runtime_function_bodies(self) -> None:
        generated = generate_guc_runtime.compile_runtime(
            self.catalog(), self.policy(), self.controls()
        )
        content = "\n".join(generated.values())
        self.assertNotIn("postgamma_find_descriptor(", content)
        self.assertNotIn("postgamma_copy_guc_value(", content)
        self.assertNotIn("postgamma_bind_builtin_guc_variables(", content)

    def test_requires_one_configure_names_control(self) -> None:
        controls = self.controls()
        controls["symbols"] = []
        with self.assertRaises(check_guc_policy.PolicyError):
            generate_guc_runtime.compile_runtime(
                self.catalog(), self.policy(), controls
            )


class BackendStatePolicyTests(unittest.TestCase):
    @staticmethod
    def catalog() -> dict[str, object]:
        candidates = [
            {
                "id": "external:guc_value",
                "name": "guc_value",
                "usr": "c:@guc_value",
                "canonical_type": "int",
                "complete_type": True,
                "trivially_copyable": True,
                "size": 4,
                "alignment": 4,
                "has_initializer": True,
            },
            {
                "id": "internal:fixture.c:fixture.c::pointer",
                "name": "pointer",
                "usr": "c:fixture.c@pointer",
                "canonical_type": "int *",
                "complete_type": True,
                "trivially_copyable": True,
                "size": 8,
                "alignment": 8,
                "has_initializer": True,
            },
            {
                "id": "internal:fixture.c:fixture.c::target",
                "name": "target",
                "usr": "c:fixture.c@target",
                "canonical_type": "int",
                "complete_type": True,
                "trivially_copyable": True,
                "size": 4,
                "alignment": 4,
                "has_initializer": True,
            },
        ]
        return {
            "schema_version": 1,
            "kind": check_backend_state_policy.CATALOG_KIND,
            "candidates": candidates,
            "uses": [
                {
                    "id": "internal:fixture.c:fixture.c::target",
                    "source_kind": "source",
                    "path": "fixture.c",
                    "offset": 50,
                    "length": 6,
                    "in_static_initializer": True,
                    "use_kind": "address",
                    "static_initializer_owner_id": "internal:fixture.c:fixture.c::pointer",
                }
            ],
            "summary": {"candidate_count": len(candidates)},
        }

    @staticmethod
    def policy() -> dict[str, object]:
        return {
            "schema_version": 1,
            "kind": check_backend_state_policy.POLICY_KIND,
            "decisions": [
                {"id": "external:guc_value", "owner": "managed_guc", "rationale": "test"},
                {
                    "id": "internal:fixture.c:fixture.c::pointer",
                    "owner": "role",
                    "rationale": "test",
                },
                {
                    "id": "internal:fixture.c:fixture.c::target",
                    "owner": "role",
                    "rationale": "test",
                },
            ],
        }

    @staticmethod
    def guc_inventory() -> dict[str, object]:
        return {
            "schema_version": 1,
            "mode": "scan",
            "declarations": [
                {
                    "symbol_id": "pg.guc.guc_value",
                    "usr": "c:@guc_value",
                    "definition": True,
                }
            ],
        }

    def test_requires_exact_owners_and_emits_static_pointer_relocation(self) -> None:
        report = check_backend_state_policy.validate_alignment(
            self.catalog(), self.policy(), self.guc_inventory()
        )
        self.assertEqual(report["candidate_count"], 3)
        self.assertEqual(report["transformed_candidate_count"], 2)
        self.assertEqual(
            report["relocations"],
            [
                {
                    "owner_id": "internal:fixture.c:fixture.c::pointer",
                    "target_id": "internal:fixture.c:fixture.c::target",
                }
            ],
        )

    def test_rejects_an_unreviewed_candidate(self) -> None:
        policy = self.policy()
        policy["decisions"] = policy["decisions"][:-1]
        with self.assertRaisesRegex(
            check_backend_state_policy.BackendStatePolicyError,
            "missing ownership decision",
        ):
            check_backend_state_policy.validate_alignment(
                self.catalog(), policy, self.guc_inventory()
            )

    def test_accepts_an_absent_reviewed_conditional_candidate(self) -> None:
        policy = self.policy()
        policy["decisions"].append(
            {
                "id": "external:optional_debug_state",
                "owner": "role",
                "rationale": "present only in an optional build profile",
                "availability": "conditional",
                "condition": "OPTIONAL_DEBUG_STATE",
            }
        )
        report = check_backend_state_policy.validate_alignment(
            self.catalog(), policy, self.guc_inventory()
        )
        self.assertEqual(report["dormant_conditional_count"], 1)
        self.assertEqual(
            report["dormant_conditional_decisions"],
            ["external:optional_debug_state"],
        )

    def test_generates_deterministic_aligned_opaque_slots(self) -> None:
        report = check_backend_state_policy.validate_alignment(
            self.catalog(), self.policy(), self.guc_inventory()
        )
        slots, sizes, counts = generate_backend_state_runtime.compile_slots(
            self.catalog(), self.policy(), report
        )
        self.assertEqual(counts, {"instance": 0, "role": 2, "session": 0})
        self.assertEqual(sizes["role"] % 8, 0)
        self.assertEqual(sum(bool(slot.relocations) for slot in slots), 1)
        layout = generate_backend_state_runtime.emit_layout_header(
            slots, sizes, counts
        )
        descriptors = generate_backend_state_runtime.emit_descriptor_initializers(
            slots
        )
        self.assertIn("typedef enum PostgammaBackendStateSlot", layout)
        self.assertIn("postgamma_backend_state_relocated_", layout)
        self.assertIn("POSTGAMMA_BACKEND_STATE_OWNER_ROLE", descriptors)
        self.assertIn("internal:fixture.c:fixture.c::pointer", descriptors)

    def test_instance_owner_is_a_generated_runtime_slot(self) -> None:
        catalog = self.catalog()
        policy = self.policy()
        policy["decisions"][1]["owner"] = "instance"
        policy["decisions"][2]["owner"] = "instance"
        report = check_backend_state_policy.validate_alignment(
            catalog, policy, self.guc_inventory()
        )
        slots, sizes, counts = generate_backend_state_runtime.compile_slots(
            catalog, policy, report
        )
        self.assertEqual(counts, {"instance": 2, "role": 0, "session": 0})
        self.assertGreater(sizes["instance"], 0)
        descriptors = generate_backend_state_runtime.emit_descriptor_initializers(
            slots
        )
        self.assertIn("POSTGAMMA_BACKEND_STATE_OWNER_INSTANCE", descriptors)

    def test_generator_emits_facts_without_runtime_function_bodies(self) -> None:
        report = check_backend_state_policy.validate_alignment(
            self.catalog(), self.policy(), self.guc_inventory()
        )
        slots, sizes, counts = generate_backend_state_runtime.compile_slots(
            self.catalog(), self.policy(), report
        )
        generated = "\n".join(
            [
                generate_backend_state_runtime.emit_layout_header(
                    slots, sizes, counts
                ),
                generate_backend_state_runtime.emit_descriptor_initializers(slots),
            ]
        )
        self.assertNotIn("postgamma_backend_state_context_init(", generated)
        self.assertNotIn("postgamma_backend_state_address_internal(", generated)
        self.assertNotIn("postgamma_backend_state_context_copy_ids(", generated)


class CombinedPlanTests(unittest.TestCase):
    def test_merges_non_overlapping_semantic_edits(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            plans = []
            for index, offset in enumerate((1, 4)):
                plan = {
                    "schema_version": 1,
                    "mode": "plan",
                    "kind": f"fixture.{index}",
                    "files": [
                        {
                            "path": "sample.c",
                            "sha256": "same",
                            "replacements": [
                                {
                                    "offset": offset,
                                    "length": 1,
                                    "original": "x",
                                    "replacement": str(index),
                                    "rule_id": str(index),
                                }
                            ],
                        }
                    ],
                }
                path = root / f"plan-{index}.json"
                path.write_text(json.dumps(plan), encoding="utf-8")
                plans.append(path)
            result = merge_replacement_plans.merge(plans)
            self.assertEqual(result["summary"]["replacement_count"], 2)

    def test_declaration_scanner_handles_casts_and_initializers(self) -> None:
        source = b"static int target;\nstatic int *pointer = (int *) &target;\n"
        start = source.index(b"pointer") + len("pointer")
        self.assertEqual(
            source[generate_backend_state_plan.declaration_terminator(source, start) - 1 :],
            b";\n",
        )

    def test_rejects_a_shared_start_even_for_zero_length_insertion(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            plans = []
            for index, length in enumerate((0, 1)):
                plan = {
                    "schema_version": 1,
                    "mode": "plan",
                    "kind": f"fixture.{index}",
                    "files": [
                        {
                            "path": "sample.c",
                            "sha256": "same",
                            "replacements": [
                                {
                                    "offset": 4,
                                    "length": length,
                                    "original": "" if length == 0 else "x",
                                    "replacement": str(index),
                                    "rule_id": str(index),
                                }
                            ],
                        }
                    ],
                }
                path = root / f"plan-{index}.json"
                path.write_text(json.dumps(plan), encoding="utf-8")
                plans.append(path)
            with self.assertRaisesRegex(ValueError, "overlapping replacements"):
                merge_replacement_plans.merge(plans)

    def test_rejects_static_and_runtime_classification_for_same_token(self) -> None:
        catalog = BackendStatePolicyTests.catalog()
        catalog["uses"].extend(
            [
                {
                    "id": "internal:fixture.c:fixture.c::target",
                    "source_kind": "source",
                    "path": "fixture.c",
                    "offset": 12,
                    "length": 6,
                    "in_static_initializer": False,
                },
                {
                    "id": "internal:fixture.c:fixture.c::target",
                    "source_kind": "source",
                    "path": "fixture.c",
                    "offset": 12,
                    "length": 6,
                    "in_static_initializer": True,
                    "static_initializer_owner_id": "internal:fixture.c:fixture.c::pointer",
                },
            ]
        )
        report = check_backend_state_policy.validate_alignment(
            catalog, BackendStatePolicyTests.policy(), BackendStatePolicyTests.guc_inventory()
        )
        slots, _sizes, _counts = generate_backend_state_runtime.compile_slots(
            catalog, BackendStatePolicyTests.policy(), report
        )
        runtime = {
            "schema_version": 1,
            "kind": generate_backend_state_plan.RUNTIME_KIND,
            "slots": [
                {
                    "id": slot.identifier,
                    "name": slot.name,
                    "enum": slot.enum_name,
                    "relocations": slot.relocations,
                }
                for slot in slots
            ],
        }
        with tempfile.TemporaryDirectory() as temporary:
            with self.assertRaisesRegex(ValueError, "both static-initializer and runtime"):
                generate_backend_state_plan.compile_plan(
                    catalog,
                    BackendStatePolicyTests.policy(),
                    runtime,
                    Path(temporary),
                )


class GeneratedArtifactTests(unittest.TestCase):
    def test_write_artifact_is_idempotent(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            target = Path(temporary) / "parser.c"
            install_generated_artifacts.write_artifact(target, b"first\n", 0o100644)
            before = target.stat().st_mtime_ns
            install_generated_artifacts.write_artifact(target, b"first\n", 0o100644)
            self.assertEqual(target.stat().st_mtime_ns, before)
            install_generated_artifacts.write_artifact(target, b"second\n", 0o100644)
            self.assertEqual(target.read_bytes(), b"second\n")


class ToolchainTests(unittest.TestCase):
    def test_preserves_clangxx_driver_symlink_name(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            clang = root / "clang"
            clang.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
            clang.chmod(0o755)
            clangxx = root / "clang++"
            clangxx.symlink_to(clang.name)
            self.assertEqual(toolchain._executable(clangxx), str(clangxx.absolute()))


class CompileSourceDomainTests(unittest.TestCase):
    def test_zero_match_assumption_scans_the_complete_declared_domain(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            clean = root / "clean.c"
            forbidden = root / "forbidden.c"
            clean.write_text("int clean(void) { return 0; }\n", encoding="utf-8")
            forbidden.write_text(
                "int forbidden_call(void);\n"
                "int use_forbidden(void) { return forbidden_call(); }\n",
                encoding="utf-8",
            )
            sources = [
                ("source", clean, "clean.c"),
                ("source", forbidden, "forbidden.c"),
            ]
            selected, tokens, strategy = list_compile_sources.select_candidates(
                sources,
                {
                    "symbols": [],
                    "injections": [],
                    "assumptions": [
                        {
                            "id": "forbid.call",
                            "callee": "forbidden_call",
                            "expected_matches": 0,
                            "allowed_source_file_suffixes": ["clean.c"],
                        }
                    ],
                },
            )
        self.assertEqual(selected, sources)
        self.assertEqual(tokens, ["forbidden_call"])
        self.assertEqual(strategy, "full-domain-zero-match-assumption")

    def test_extracts_split_and_joined_preprocessor_defines(self) -> None:
        import filter_compile_database

        self.assertEqual(
            filter_compile_database.defined_macros(
                ["cc", "-DFRONTEND", "-D", "USE_PRIVATE_ENCODING_FUNCS=1"]
            ),
            {"FRONTEND", "USE_PRIVATE_ENCODING_FUNCS"},
        )

    def test_detects_frontend_domain_from_umbrella_header(self) -> None:
        import filter_compile_database

        with tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary) / "frontend.c"
            source.write_text(
                '#include "postgres_fe.h"\n#define LOCAL_DOMAIN 1\n',
                encoding="utf-8",
            )
            self.assertEqual(
                filter_compile_database.source_defined_macros(source),
                {"FRONTEND", "LOCAL_DOMAIN"},
            )

    def test_keeps_command_profile_for_dual_mode_common_source(self) -> None:
        import filter_compile_database

        with tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary) / "common.c"
            source.write_text(
                '#ifndef FRONTEND\n#include "postgres.h"\n#else\n'
                '#include "postgres_fe.h"\n#endif\n',
                encoding="utf-8",
            )
            self.assertEqual(
                filter_compile_database.source_defined_macros(source),
                set(),
            )

    def test_server_domain_excludes_frontend_programs(self) -> None:
        prefixes = ["src/backend/", "src/pl/"]
        files = ["src/test/regress/regress.c"]
        self.assertTrue(
            list_compile_sources.in_translation_unit_domain(
                "src/backend/utils/misc/guc.c", prefixes, files
            )
        )
        self.assertTrue(
            list_compile_sources.in_translation_unit_domain(
                "src/test/regress/regress.c", prefixes, files
            )
        )
        self.assertFalse(
            list_compile_sources.in_translation_unit_domain(
                "src/bin/initdb/initdb.c", prefixes, files
            )
        )

    def test_classifies_configured_build_sources_and_canonicalizes_symlinks(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            source_root = root / "postgres"
            generated_root = root / "reference"
            source = source_root / "src/backend/posix_sema.c"
            generated = generated_root / "src/backend/parser/scan.c"
            source.parent.mkdir(parents=True)
            generated.parent.mkdir(parents=True)
            source.write_text("int source;\n", encoding="utf-8")
            generated.write_text("int generated;\n", encoding="utf-8")
            alias = generated_root / "src/backend/pg_sema.c"
            alias.symlink_to(source)

            self.assertEqual(
                list_compile_sources.source_root_kind(
                    generated, source_root, generated_root
                ),
                "generated_build",
            )
            normalized = merge_compile_db.canonicalize_source_argument(
                ["cc", "-c", str(alias)], generated_root, alias.resolve()
            )
            self.assertEqual(normalized[-1], str(source.resolve()))


if __name__ == "__main__":
    unittest.main()
