"""Tests for deterministic PostgreSQL BKI preparation."""

from __future__ import annotations

import sys
import tempfile
import unittest
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(PROJECT_ROOT / "buildsys"))

import prepare_embedded_bootstrap  # noqa: E402


class PrepareEmbeddedBootstrapTests(unittest.TestCase):
    def test_prepares_every_required_token_and_reports_counts(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            bki = root / "postgres.bki"
            config = root / "pg_config.h"
            manual = root / "pg_config_manual.h"
            encoding = root / "pg_wchar.h"
            output = root / "prepared.bki"
            report = root / "report.json"
            bki.write_text(
                "# PostgreSQL 19\n"
                "NAMEDATALEN SIZEOF_POINTER ALIGNOF_POINTER POSTGRES ENCODING "
                "LC_COLLATE LC_CTYPE DATLOCALE ICU_RULES LOCALE_PROVIDER\n",
                encoding="utf-8",
            )
            config.write_text("#define SIZEOF_VOID_P 8\n", encoding="utf-8")
            manual.write_text("#define NAMEDATALEN 64\n", encoding="utf-8")
            encoding.write_text(
                "typedef enum pg_enc { PG_SQL_ASCII = 0, PG_LATIN1, PG_UTF8 } pg_enc;\n",
                encoding="utf-8",
            )

            document = prepare_embedded_bootstrap.prepare(
                bki, config, manual, encoding, output, report
            )

            self.assertEqual(document["configuration"]["pointer_size"], 8)
            self.assertEqual(
                {item["count"] for item in document["replacements"].values()},
                {1},
            )
            self.assertEqual(
                output.read_text(encoding="utf-8").splitlines()[1],
                "64 8 d postgamma 2 C C _null_ _null_ c",
            )

    def test_rejects_an_incompatible_bki_major(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            bki = root / "postgres.bki"
            config = root / "pg_config.h"
            manual = root / "pg_config_manual.h"
            encoding = root / "pg_wchar.h"
            bki.write_text("# PostgreSQL 20\n", encoding="utf-8")
            config.write_text("#define SIZEOF_VOID_P 8\n", encoding="utf-8")
            manual.write_text("#define NAMEDATALEN 64\n", encoding="utf-8")
            encoding.write_text(
                "typedef enum pg_enc { PG_SQL_ASCII = 0, PG_UTF8 } pg_enc;\n",
                encoding="utf-8",
            )
            with self.assertRaisesRegex(
                prepare_embedded_bootstrap.BootstrapInputError, "PostgreSQL 19"
            ):
                prepare_embedded_bootstrap.prepare(
                    bki, config, manual, encoding, root / "out", root / "report"
                )


if __name__ == "__main__":
    unittest.main()
