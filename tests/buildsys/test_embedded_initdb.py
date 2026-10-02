from __future__ import annotations

import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "buildsys"))

from check_embedded_initdb import (  # noqa: E402
    CLUSTER_MARKER,
    InitdbCheckError,
    audit_runtime,
    require_marker,
    staging_artifacts,
    staging_directories,
)


CLEAN_RUNTIME_RECORD = (
    "POSTGAMMA_RUNTIME backend_model=thread threads_started=true "
    "role_process_launches=0 forbidden_process_launch_attempts=0 "
    "unsupported_role_requests=0 role_completions=8 "
    "role_threads_active=0\n"
)


class EmbeddedInitdbEvidenceTests(unittest.TestCase):
    def test_accepts_complete_cluster_marker(self) -> None:
        values = require_marker(
            "POSTGAMMA_CLUSTER_CREATE clusters=2 "
            "unique_identifiers=true same_process=true reopen=true "
            "plpgsql=true snowball=true checksums=true open_modes=true phase=closed\n",
            CLUSTER_MARKER,
        )
        self.assertEqual(values["clusters"], "2")
        self.assertEqual(values["reopen"], "true")

    def test_accepts_four_clean_runtime_records(self) -> None:
        evidence = audit_runtime(CLEAN_RUNTIME_RECORD * 4)
        self.assertEqual(evidence["runtime_records"], 4)
        self.assertEqual(evidence["total_role_completions"], 32)

    def test_rejects_missing_or_extra_runtime_records(self) -> None:
        for count in (0, 1, 2, 3, 5):
            with self.subTest(count=count):
                with self.assertRaisesRegex(
                    InitdbCheckError,
                    f"expected 4 same-process runtime records, found {count}",
                ):
                    audit_runtime(CLEAN_RUNTIME_RECORD * count)

    def test_rejects_unsafe_telemetry_in_each_runtime_record(self) -> None:
        for field, valid, invalid in (
            ("backend_model", "thread", "process"),
            ("threads_started", "true", "false"),
            ("role_process_launches", "0", "1"),
            ("forbidden_process_launch_attempts", "0", "1"),
            ("unsupported_role_requests", "0", "1"),
            ("role_threads_active", "0", "1"),
        ):
            for index in range(4):
                with self.subTest(field=field, record=index + 1):
                    records = [CLEAN_RUNTIME_RECORD] * 4
                    records[index] = records[index].replace(
                        f"{field}={valid}", f"{field}={invalid}"
                    )
                    with self.assertRaisesRegex(InitdbCheckError, field):
                        audit_runtime("".join(records))

    def test_rejects_invalid_completions_in_fourth_record(self) -> None:
        for replacement in ("", "role_completions=invalid "):
            with self.subTest(replacement=replacement):
                fourth = CLEAN_RUNTIME_RECORD.replace(
                    "role_completions=8 ", replacement
                )
                with self.assertRaisesRegex(
                    InitdbCheckError, "invalid role completion telemetry"
                ):
                    audit_runtime(CLEAN_RUNTIME_RECORD * 3 + fourth)

    def test_rejects_fatal_errors_with_four_clean_runtime_records(self) -> None:
        for severity in ("FATAL", "PANIC"):
            with self.subTest(severity=severity):
                with self.assertRaisesRegex(InitdbCheckError, "fatal backend error"):
                    audit_runtime(CLEAN_RUNTIME_RECORD * 4 + f"{severity}: test error\n")

    def test_staging_inventory_matches_only_owned_directory_shape(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            target = root / "cluster"
            expected = root / ".cluster.postgamma-create.A1b2C3"
            expected.mkdir()
            (root / ".cluster.postgamma-create.too-long").mkdir()
            (root / ".other.postgamma-create.A1b2C3").mkdir()
            self.assertEqual(staging_directories(target), [expected])
            owner = Path(str(expected) + ".owner")
            owner.write_text(f"{target}\n", encoding="utf-8")
            self.assertEqual(staging_artifacts(target), [expected, owner])


if __name__ == "__main__":
    unittest.main()
