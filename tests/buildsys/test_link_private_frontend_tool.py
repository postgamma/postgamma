from __future__ import annotations

import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "buildsys"))

from link_private_frontend_tool import (  # noqa: E402
    PrivateFrontendToolLinkError,
    link,
)


@unittest.skipUnless(
    all(shutil.which(tool) for tool in ("cc", "ar", "objcopy", "nm")),
    "ELF build tools are required",
)
class PrivateFrontendToolLinkTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def compile(self, name: str, source: str) -> Path:
        source_path = self.root / f"{name}.c"
        object_path = self.root / f"{name}.o"
        source_path.write_text(source, encoding="utf-8")
        subprocess.run(
            ["cc", "-fPIC", "-c", str(source_path), "-o", str(object_path)],
            check=True,
        )
        return object_path

    def archive(self, object_path: Path) -> Path:
        archive = self.root / "libtool.a"
        subprocess.run(["ar", "rcs", str(archive), str(object_path)], check=True)
        return archive

    def test_retains_only_the_versioned_tool_entry(self) -> None:
        entry = self.compile(
            "entry",
            "extern int pg_hidden(void);\n"
            "int helper(void) { return 2; }\n"
            "int postgamma_initdb_create(void) { return pg_hidden() + helper(); }\n",
        )
        archive = self.archive(
            self.compile("hidden", "int pg_hidden(void) { return 17; }\n")
        )
        output = self.root / "private-initdb.o"
        receipt = self.root / "receipt.json"
        document = link(
            "cc",
            "objcopy",
            "nm",
            "initdb",
            [entry],
            [archive],
            ("postgamma_initdb_create",),
            (),
            None,
            None,
            output,
            receipt,
        )
        self.assertEqual(
            document["kept_symbols"], ["postgamma_initdb_create"]
        )
        self.assertGreaterEqual(document["localized_symbol_count"], 2)
        symbols = subprocess.run(
            ["nm", "-g", "--defined-only", str(output)],
            check=True,
            text=True,
            stdout=subprocess.PIPE,
        ).stdout
        self.assertIn("postgamma_initdb_create", symbols)
        self.assertNotIn("pg_hidden", symbols)
        self.assertTrue(receipt.is_file())

    def test_rejects_a_missing_entry_symbol(self) -> None:
        entry = self.compile("entry", "int helper(void) { return 2; }\n")
        archive = self.archive(
            self.compile("hidden", "int pg_hidden(void) { return 17; }\n")
        )
        with self.assertRaisesRegex(
            PrivateFrontendToolLinkError, "entry symbol"
        ):
            link(
                "cc",
                "objcopy",
                "nm",
                "initdb",
                [entry],
                [archive],
                ("postgamma_initdb_create",),
                (),
                None,
                None,
                self.root / "private-initdb.o",
                self.root / "receipt.json",
            )

    def test_namespaces_an_archive_reference(self) -> None:
        entry = self.compile(
            "entry",
            "extern int PQprobe(void);\n"
            "int local_helper(void) { return 1; }\n"
            "int postgamma_tool_entry(void) "
            "{ return PQprobe() + local_helper(); }\n",
        )
        archive = self.archive(
            self.compile("namespace", "int PQprobe(void) { return 23; }\n")
        )
        output = self.root / "private-tool.o"
        receipt = self.root / "namespace-receipt.json"
        document = link(
            "cc",
            "objcopy",
            "nm",
            "logical-tool",
            [entry],
            [],
            ("postgamma_tool_entry",),
            (),
            archive,
            "postgamma_private_symbol_",
            output,
            receipt,
        )
        self.assertEqual(
            document["namespaced_symbols"],
            {"PQprobe": "postgamma_private_symbol_PQprobe"},
        )
        undefined = subprocess.run(
            ["nm", "-u", str(output)],
            check=True,
            text=True,
            stdout=subprocess.PIPE,
        ).stdout
        self.assertIn("postgamma_private_symbol_PQprobe", undefined)
        self.assertNotIn(" U PQprobe", undefined)


if __name__ == "__main__":
    unittest.main()
