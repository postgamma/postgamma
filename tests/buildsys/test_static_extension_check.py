"""Tests for the hidden static-extension executable evidence gate."""

from __future__ import annotations

import json
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock


PROJECT_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(PROJECT_ROOT / "buildsys"))

import check_static_extension  # noqa: E402
import link_bootstrap_static_extension  # noqa: E402
import postgresql_embedded_adapter  # noqa: E402


class StaticExtensionCheckTests(unittest.TestCase):
    adapter_path = PROJECT_ROOT / "manifests/postgresql/embedded.json"

    def test_current_pg19_provider_seam_matches_the_adapter(self) -> None:
        adapter = postgresql_embedded_adapter.load_adapter(self.adapter_path)
        report = check_static_extension.verify_provider_seam(
            PROJECT_ROOT / "postgres", adapter
        )
        self.assertEqual(report["enclosing_function"], "internal_load_library")
        self.assertEqual(
            report["callee_matches"],
            {"dlclose": 2, "dlopen": 1, "dlsym": 2, "stat": 1},
        )

    def test_provider_seam_fails_closed_on_callee_drift(self) -> None:
        adapter = postgresql_embedded_adapter.load_adapter(self.adapter_path)
        adapter["module_provider_seam"]["required_callees"][0][
            "expected_matches"
        ] += 1
        with self.assertRaisesRegex(
            check_static_extension.StaticExtensionCheckError, "expected"
        ):
            check_static_extension.verify_provider_seam(
                PROJECT_ROOT / "postgres", adapter
            )

    def test_symbol_parser_removes_elf_version_suffixes(self) -> None:
        output = "pgm_probe T 1 1\nmemcpy@GLIBC_2.14 U\n"
        with mock.patch.object(check_static_extension, "run", return_value=output):
            symbols = check_static_extension.nm_symbols(
                "nm", Path("fixture.so"), dynamic=True, defined=True
            )
        self.assertEqual(symbols, {"pgm_probe", "memcpy"})

    def test_dynamic_loader_audit_ignores_comments_but_counts_calls(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary) / "provider.c"
            source.write_text(
                "/* dlopen(ignored) */\nvoid probe(void) { dlsym(0, 0); }\n",
                encoding="utf-8",
            )
            counts = check_static_extension.count_direct_dynamic_loader_calls(
                [source]
            )
        self.assertEqual(counts, {"dlclose": 0, "dlopen": 0, "dlsym": 1})

    def test_syscall_audit_rejects_host_signal_delivery(self) -> None:
        trace = (
            '1 execve("/tmp/static-driver", ["/tmp/static-driver"], 0) = 0\n'
            "1 kill(1, SIGCHLD) = 0\n"
        )
        with self.assertRaisesRegex(
            check_static_extension.StaticExtensionCheckError, "host signal"
        ):
            check_static_extension.audit_trace(
                trace, Path("/tmp/static-driver")
            )

    def test_link_receipt_rejects_a_stale_input(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            library = root / "library.so"
            link_input = root / "input.o"
            library.write_bytes(b"library")
            link_input.write_bytes(b"first")
            receipt = {
                "schema_version": 1,
                "kind": link_bootstrap_static_extension.RECEIPT_KIND,
                "command": ["cc", *link_bootstrap_static_extension.REQUIRED_FLAGS],
                "work_directory": str(root),
                "required_flags": list(
                    link_bootstrap_static_extension.REQUIRED_FLAGS
                ),
                "inputs": {
                    str(link_input): check_static_extension.sha256(link_input)
                },
                "output": str(library),
                "output_sha256": check_static_extension.sha256(library),
            }
            link_input.write_bytes(b"second")
            with self.assertRaisesRegex(
                check_static_extension.StaticExtensionCheckError, "stale"
            ):
                check_static_extension.verify_link_receipt(receipt, library)

    def test_link_command_contains_every_required_safety_flag(self) -> None:
        command = link_bootstrap_static_extension.link_command(
            "cc",
            [Path("registry.o")],
            Path("extension.a"),
            Path("kernel.rsp"),
            Path("exports.map"),
            Path("library.so"),
        )
        for flag in link_bootstrap_static_extension.REQUIRED_FLAGS:
            self.assertIn(flag, command)
        self.assertLess(command.index("registry.o"), command.index("extension.a"))
        self.assertLess(command.index("extension.a"), command.index("@kernel.rsp"))

    def test_embedded_adapter_rejects_a_missing_provider_seam(self) -> None:
        document = json.loads(self.adapter_path.read_text(encoding="utf-8"))
        del document["module_provider_seam"]
        with self.assertRaisesRegex(
            postgresql_embedded_adapter.EmbeddedAdapterError,
            "module_provider_seam",
        ):
            postgresql_embedded_adapter.validate_adapter(document)

    def test_embedded_adapter_rejects_a_missing_bootstrap_seam(self) -> None:
        document = json.loads(self.adapter_path.read_text(encoding="utf-8"))
        del document["bootstrap_seam"]
        with self.assertRaisesRegex(
            postgresql_embedded_adapter.EmbeddedAdapterError,
            "bootstrap_seam",
        ):
            postgresql_embedded_adapter.validate_adapter(document)


if __name__ == "__main__":
    unittest.main()
