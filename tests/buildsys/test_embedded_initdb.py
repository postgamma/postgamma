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


class EmbeddedInitdbEvidenceTests(unittest.TestCase):
    def test_accepts_complete_cluster_marker(self) -> None:
        values = require_marker(
            "POSTGAMMA_CLUSTER_CREATE clusters=2 "
            "unique_identifiers=true same_process=true reopen=true "
            "plpgsql=true snowball=true checksums=true phase=closed\n",
            CLUSTER_MARKER,
        )
        self.assertEqual(values["clusters"], "2")
        self.assertEqual(values["reopen"], "true")

    def test_accepts_three_clean_runtime_records(self) -> None:
        record = (
            "POSTGAMMA_RUNTIME backend_model=thread threads_started=true "
            "role_process_launches=0 forbidden_process_launch_attempts=0 "
            "unsupported_role_requests=0 role_completions=8 "
            "role_threads_active=0\n"
        )
        evidence = audit_runtime(record * 3)
        self.assertEqual(evidence["runtime_records"], 3)
        self.assertEqual(evidence["total_role_completions"], 24)

    def test_rejects_a_role_process_launch(self) -> None:
        record = (
            "POSTGAMMA_RUNTIME backend_model=thread threads_started=true "
            "role_process_launches=1 forbidden_process_launch_attempts=0 "
            "unsupported_role_requests=0 role_completions=8 "
            "role_threads_active=0\n"
        )
        with self.assertRaisesRegex(InitdbCheckError, "role_process_launches"):
            audit_runtime(record * 3)

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
