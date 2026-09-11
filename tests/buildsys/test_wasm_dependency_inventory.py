"""Native link evidence must not be mistaken for WebAssembly support."""

from __future__ import annotations

from collections import defaultdict
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest


PROJECT_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(PROJECT_ROOT / "buildsys"))

import wasm_dependency_inventory as inventory  # noqa: E402


class WasmDependencyInventoryTests(unittest.TestCase):
    def test_archive_members_weak_imports_and_source_locations(self) -> None:
        records = inventory.parse_nm(
            "/work with space/lib.a[worker.o]: sem_wait U         \tworker.c:42\n"
            "/work with space/lib.a[worker.o]: optional_api w         \n"
        )
        self.assertEqual(records[0]["container"], "/work with space/lib.a[worker.o]")
        self.assertEqual(records[0]["location"], "worker.c:42")
        self.assertEqual(records[1]["binding"], "w")
        self.assertEqual(records[1]["location"], "")

    def test_unrecognized_nm_output_is_not_silently_dropped(self) -> None:
        with self.assertRaises(inventory.InventoryError):
            inventory.parse_nm("worker.o: output from an unexpected symbol-reader format\n")

    def test_nested_debug_prefix_mapping(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            build = root / "build"
            source = build / "embedded-bootstrap/generated/postgres/src/backend/port/pg_shmem.c"
            source.parent.mkdir(parents=True)
            source.write_text("native fixture\n", encoding="utf-8")
            location = inventory.resolve_location(
                "postgresql-build/postgres-pic/src/backend/port/"
                "postgresql-build/generated/postgres/src/backend/port/pg_shmem.c:157",
                root, build, {},
            )
            self.assertEqual(location["source"], source.relative_to(root).as_posix())
            self.assertEqual(location["line"], 157)

    def test_ambiguous_basename_is_not_attributed_to_one_source(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            candidates = {root / "first/worker.c", root / "second/worker.c"}
            location = inventory.resolve_location("worker.c:0", root, root / "build",
                                                  {"worker.c": candidates})
            self.assertEqual(location["status"], "ambiguous")
            self.assertNotIn("source", location)
            self.assertEqual(len(location["candidates"]), 2)

    def test_changed_and_missing_inputs_remain_mismatches(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "worker.o"
            source.write_bytes(b"original object")
            digest = inventory.sha256(source)
            self.assertEqual(inventory.compare_hash(source, digest, root, "object")["status"],
                             "matched")
            source.write_bytes(b"changed object")
            self.assertEqual(inventory.compare_hash(source, digest, root, "object")["status"],
                             "mismatch")
            source.unlink()
            self.assertIsNone(inventory.compare_hash(source, digest, root, "object")["actual"])

    def test_direct_link_inputs_follow_the_current_recipe(self) -> None:
        source = ('$(EMBEDDED_KERNEL_PUBLIC_API_STATIC_LIBRARY) \\\n'
                  '    $(RECEIPT):\n'
                  '\tpython3 buildsys/build_static_library.py \\\n'
                  '\t  --input "$(PUBLIC_OBJECT)" \\\n'
                  '\t  --input "$(NEW_PROVIDER_OBJECT)"\n'
                  '\nkernel-static-library: $(RECEIPT)\n')
        self.assertEqual(inventory.direct_input_variables(source),
                         ["PUBLIC_OBJECT", "NEW_PROVIDER_OBJECT"])
        with self.assertRaises(inventory.InventoryError):
            inventory.direct_input_variables(source.replace("NEW_PROVIDER_OBJECT", "PUBLIC_OBJECT"))
        with self.assertRaises(inventory.InventoryError):
            inventory.direct_input_variables("unrelated target:\n")

    def test_unknown_external_service_requires_review(self) -> None:
        self.assertEqual(inventory.classify_symbol("new_platform_service")["target_disposition"],
                         "unresolved")
        self.assertEqual(inventory.classify_symbol("posix_spawn_file_actions_init")["category"],
                         "process")
        self.assertEqual(inventory.classify_symbol("pthread_sigmask")["category"], "host_signal")
        self.assertEqual(inventory.classify_symbol("__sigsetjmp")["category"], "error_recovery")
        self.assertEqual(inventory.classify_symbol("shmget@GLIBC_2.2.5")["category"], "shared_memory")

    def test_missing_input_attribution_is_retained(self) -> None:
        references = defaultdict(list, {"system": [{"object": "worker.o"}]})
        rows = inventory.dependency_records({"system", "new_external_service"}, references)
        missing = next(row for row in rows if row["symbol"] == "new_external_service")
        self.assertTrue(missing["at_native_library_boundary"])
        self.assertEqual(missing["references"], [])
        self.assertEqual(missing["target_disposition"], "unresolved")
        self.assertNotIn("new_external_service", references)

    def test_cli_retains_an_existing_report(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            report = root / "report.json"
            content = '{"prior_evidence": true}\n'
            report.write_text(content, encoding="utf-8")
            result = subprocess.run([sys.executable, str(Path(inventory.__file__)),
                                     "--root", str(root), "--output", str(report)],
                                    text=True, capture_output=True, check=False)
            self.assertEqual(result.returncode, 2)
            self.assertIn("output already exists", result.stderr)
            self.assertEqual(report.read_text(encoding="utf-8"), content)

    def test_source_hints_exclude_comments_and_string_literals(self) -> None:
        source = ('int\nquery(void)\n{\n  /* system("ignored"); */\n'
                  '  const char *text = "system(fake)";\n  return 0;\n}\n'
                  'int\ncompatibility(const char *command)\n{\n  return system(command);\n}\n')
        scopes = inventory.function_scopes(source)
        hint = inventory.source_callers(inventory.sanitize_c(source), "system", 0, scopes)
        self.assertEqual(hint["kind"], "current-source-candidates")
        self.assertEqual(hint["functions"], ["compatibility"])

    @unittest.skipUnless(shutil.which("cc") and shutil.which("nm"), "requires native C and nm")
    def test_runtime_guard_does_not_remove_native_import(self) -> None:
        with tempfile.TemporaryDirectory(prefix="wasm inventory ") as temporary:
            root = Path(temporary)
            source = root / "guarded.c"
            output = root / "guarded.o"
            source.write_text(
                'extern int system(const char *);\n'
                'int\nguarded(int embedded, const char *command)\n{\n'
                '  if (embedded)\n    return -1;\n  return system(command);\n}\n',
                encoding="utf-8",
            )
            subprocess.run(["cc", "-O2", "-g", "-c", str(source), "-o", str(output)], check=True,
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            references = inventory.nm_references([output], root, "nm", locations=True)
            system = [record for record in references if record["symbol"] == "system"]
            self.assertEqual(len(system), 1)
            self.assertEqual(inventory.classify_symbol(system[0]["symbol"])["target_disposition"],
                             "excluded")
            location = inventory.resolve_location(system[0]["location"], root, root / "build", {})
            self.assertEqual(location["source"], "guarded.c")
            self.assertGreater(location["line"], 0)


if __name__ == "__main__":
    unittest.main()
