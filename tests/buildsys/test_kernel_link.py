"""Tests for deriving the embedded kernel closure from PostgreSQL's real link."""

from __future__ import annotations

import json
import sys
import tempfile
import unittest
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(PROJECT_ROOT / "buildsys"))

import cc_capture  # noqa: E402
import derive_kernel_link  # noqa: E402
import merge_link_db  # noqa: E402


class KernelLinkTests(unittest.TestCase):
    embedded_adapter = PROJECT_ROOT / "manifests/postgresql/embedded.json"

    @staticmethod
    def write_json(path: Path, document: dict[str, object]) -> None:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(document, sort_keys=True) + "\n", encoding="utf-8")

    def fixture(
        self, root: Path
    ) -> tuple[dict[str, object], Path, Path, Path, Path]:
        build = root / "reference"
        backend = build / "src/backend"
        main = backend / "main/main.o"
        worker = backend / "worker with space.o"
        fmgrtab = backend / "utils/fmgrtab.o"
        rmgr = backend / "access/transam/rmgr.o"
        archive = build / "src/common/libpgcommon_srv.a"
        for path in (main, worker, fmgrtab, rmgr, archive):
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(path.name.encode("utf-8"))
        configure = build / ".postgamma-configure.json"
        self.write_json(
            configure, {"schema_version": 1, "arguments": [], "environment": {}}
        )
        config_log = build / "config.log"
        config_log.write_text("fixture compiler 1.0\n", encoding="utf-8")
        upstream = root / "upstream.json"
        self.write_json(upstream, {"commit": "1" * 40})
        fragments = root / "fragments"
        fragments.mkdir()
        self.write_json(
            fragments / "backend.json",
            {
                "schema_version": 1,
                "kind": cc_capture.LINK_FRAGMENT_KIND,
                "directory": str(backend),
                "output": str(backend / "postgres"),
                "arguments": [
                    "cc",
                    "-Wl,--export-dynamic",
                    str(main),
                    str(worker),
                    str(fmgrtab),
                    str(rmgr),
                    str(archive),
                    "-Wl,-rpath,/captured/install/lib,--enable-new-dtags",
                    "-lm",
                    "-o",
                    str(backend / "postgres"),
                ],
            },
        )
        database = merge_link_db.merge_fragments(
            fragments, build, configure, config_log
        )
        return database, build, configure, config_log, upstream

    def test_excludes_only_process_main_and_preserves_order(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            database, build, configure, config_log, upstream = self.fixture(
                Path(temporary)
            )
            closure, report = derive_kernel_link.derive(
                database,
                build,
                configure,
                config_log,
                upstream,
                derive_kernel_link.load_json(self.embedded_adapter),
            )
        self.assertNotIn("main/main.o", " ".join(closure))
        self.assertNotIn(" -o ", " " + " ".join(closure) + " ")
        self.assertTrue(any("worker with space.o" in value for value in closure))
        self.assertEqual(closure[-1], "-lm")
        self.assertFalse(any("rpath" in value for value in closure))
        self.assertEqual(len(report["omitted_runtime_paths"]), 1)
        self.assertEqual(report["excluded"][0]["reason"], "standalone-process-main")
        self.assertEqual(report["unresolved_symbols"]["status"], "not-linked")
        self.assertEqual(report["catalog_registration_roots"]["status"], "matched")
        self.assertEqual(len(report["catalog_registration_roots"]["objects"]), 2)
        self.assertIn("'", derive_kernel_link.response_content(closure))

    def test_requires_the_current_configure_identity(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            database, build, configure, config_log, upstream = self.fixture(
                Path(temporary)
            )
            self.write_json(
                configure,
                {"schema_version": 1, "arguments": ["changed"], "environment": {}},
            )
            with self.assertRaisesRegex(
                derive_kernel_link.KernelLinkError, "configure identity is stale"
            ):
                derive_kernel_link.derive(
                    database,
                    build,
                    configure,
                    config_log,
                    upstream,
                    derive_kernel_link.load_json(self.embedded_adapter),
                )

    def test_requires_the_current_toolchain_identity(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            database, build, configure, config_log, upstream = self.fixture(
                Path(temporary)
            )
            config_log.write_text("fixture compiler 2.0\n", encoding="utf-8")
            with self.assertRaisesRegex(
                derive_kernel_link.KernelLinkError, "toolchain identity is stale"
            ):
                derive_kernel_link.derive(
                    database,
                    build,
                    configure,
                    config_log,
                    upstream,
                    derive_kernel_link.load_json(self.embedded_adapter),
                )

    def test_requires_exactly_one_standalone_main_object(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            database, build, configure, config_log, upstream = self.fixture(
                Path(temporary)
            )
            command = database["commands"][0]
            command["ordered_inputs"] = [
                item
                for item in command["ordered_inputs"]
                if not item["path"].endswith("/main/main.o")
            ]
            with self.assertRaisesRegex(
                derive_kernel_link.KernelLinkError, "kernel exclusion"
            ):
                derive_kernel_link.derive(
                    database,
                    build,
                    configure,
                    config_log,
                    upstream,
                    derive_kernel_link.load_json(self.embedded_adapter),
                )


if __name__ == "__main__":
    unittest.main()
