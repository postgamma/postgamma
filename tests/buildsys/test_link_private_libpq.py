from __future__ import annotations

import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "buildsys"))

from link_private_libpq import PrivateLibpqLinkError, link
from check_private_libpq import validate_link_report


@unittest.skipUnless(
    all(shutil.which(tool) for tool in ("cc", "ar", "objcopy", "nm")),
    "ELF build tools are required",
)
class PrivateLibpqLinkTests(unittest.TestCase):
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
        archive = self.root / "libprivate.a"
        subprocess.run(["ar", "rcs", str(archive), str(object_path)], check=True)
        return archive

    def test_localizes_non_bridge_definitions(self) -> None:
        entry = self.compile(
            "entry",
            "extern int pq_hidden(void);\n"
            "int postgamma_private_libpq_probe(void) { return pq_hidden(); }\n",
        )
        archive = self.archive(
            self.compile("hidden", "int pq_hidden(void) { return 17; }\n")
        )
        output = self.root / "private.o"
        receipt = self.root / "receipt.json"
        document = link(
            "cc",
            "objcopy",
            "nm",
            [entry],
            [archive],
            ("postgamma_private_libpq_",),
            ("postgamma_private_libpq_probe",),
            (),
            None,
            None,
            output,
            receipt,
        )
        self.assertEqual(
            document["retained_symbols"], ["postgamma_private_libpq_probe"]
        )
        self.assertGreaterEqual(document["localized_symbol_count"], 1)
        self.assertTrue(output.is_file())
        self.assertTrue(receipt.is_file())

    def test_rejects_missing_required_bridge(self) -> None:
        entry = self.compile("entry", "int pq_hidden(void) { return 17; }\n")
        archive = self.archive(self.compile("unused", "int unused(void) { return 1; }\n"))
        with self.assertRaisesRegex(PrivateLibpqLinkError, "bridge symbol"):
            link(
                "cc",
                "objcopy",
                "nm",
                [entry],
                [archive],
                ("postgamma_private_libpq_",),
                ("postgamma_private_libpq_probe",),
                (),
                None,
                None,
                self.root / "private.o",
                self.root / "receipt.json",
            )

    def test_namespaces_archive_definitions(self) -> None:
        entry = self.compile(
            "entry",
            "extern int PQprobe(void);\n"
            "int local_helper(void) { return 1; }\n"
            "int postgamma_private_libpq_probe(void) "
            "{ return PQprobe() + local_helper(); }\n",
        )
        archive = self.archive(
            self.compile("namespace", "int PQprobe(void) { return 29; }\n")
        )
        output = self.root / "private-namespaced.o"
        receipt = self.root / "namespace-receipt.json"
        document = link(
            "cc",
            "objcopy",
            "nm",
            [entry],
            [archive],
            ("postgamma_private_libpq_",),
            ("postgamma_private_libpq_probe",),
            (),
            archive,
            "postgamma_private_libpq_symbol_",
            output,
            receipt,
        )
        self.assertEqual(
            document["namespaced_symbols"],
            {"PQprobe": "postgamma_private_libpq_symbol_PQprobe"},
        )
        validate_link_report(document, output)
        symbols = subprocess.run(
            ["nm", "-g", "--defined-only", str(output)],
            check=True,
            text=True,
            stdout=subprocess.PIPE,
        ).stdout
        self.assertIn("postgamma_private_libpq_symbol_PQprobe", symbols)
        self.assertNotIn(" PQprobe", symbols)


if __name__ == "__main__":
    unittest.main()
