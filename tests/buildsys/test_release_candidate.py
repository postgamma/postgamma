"""Tests for the one-command local release-candidate seal."""

from __future__ import annotations

import hashlib
import json
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "buildsys"))

import check_release_candidate  # noqa: E402


def write_json(path: Path, value: object) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value), encoding="utf-8")


class ReleaseCandidateTests(unittest.TestCase):
    def fixture(self, root: Path) -> dict[str, Path]:
        paths = {
            "python_release": root / "reports/python-release.json",
            "conformance": root / "reports/conformance.json",
            "static_evidence": root / "reports/static-sdk.json",
            "static_receipt": root / "dist/static.json",
            "static_archive": root / "dist/sdk.tar.gz",
            "static_checksum": root / "dist/sdk.tar.gz.sha256",
            "wheel_receipt": root / "reports/wheel-build.json",
            "wheel_evidence": root / "reports/wheel.json",
            "wheel_directory": root / "wheel",
            "docs_evidence": root / "reports/docs.json",
            "publication_evidence": root / "reports/publication.json",
            "artifact_publication_evidence": root / "reports/artifacts.json",
            "docs_site": root / "site",
        }
        paths["static_archive"].parent.mkdir(parents=True, exist_ok=True)
        paths["static_archive"].write_bytes(b"static-sdk")
        static_digest = hashlib.sha256(b"static-sdk").hexdigest()
        paths["static_checksum"].write_text(
            f"{static_digest}  sdk.tar.gz\n", encoding="ascii"
        )
        paths["wheel_directory"].mkdir(parents=True)
        wheel = paths["wheel_directory"] / "postgamma-0.1.0a1-cp312.whl"
        wheel.write_bytes(b"wheel")
        wheel_digest = hashlib.sha256(b"wheel").hexdigest()
        paths["docs_site"].mkdir(parents=True)
        (paths["docs_site"] / "index.html").write_text("index", encoding="utf-8")
        write_json(
            paths["python_release"],
            {
                "kind": "postgamma.python-release-candidate",
                "status": "pass",
                "release": {"version": "0.1.0a1"},
                "local_wheel": {"sha256": wheel_digest},
                "inputs": {"embedded_release": "present"},
            },
        )
        write_json(
            paths["conformance"],
            {
                "kind": "postgamma.embedded-conformance",
                "status": "pass",
                "case_count": 54,
                "socket_libpq_matches_embedded_public_api": True,
            },
        )
        write_json(
            paths["static_evidence"],
            {
                "kind": "postgamma.static-sdk-evidence",
                "status": "pass",
                "archive": {"sha256": hashlib.sha256(b"library").hexdigest()},
            },
        )
        write_json(
            paths["static_receipt"],
            {
                "kind": "postgamma.static-sdk-archive",
                "status": "pass",
                "product_version": "0.1.0a1",
                "archive": "sdk.tar.gz",
                "archive_sha256": static_digest,
                "static_library_sha256": hashlib.sha256(b"library").hexdigest(),
            },
        )
        write_json(
            paths["wheel_receipt"],
            {
                "schema_version": 2,
                "kind": "postgamma.python-wheel-build",
                "version": "0.1.0a1",
                "filename": wheel.name,
                "sha256": wheel_digest,
            },
        )
        write_json(
            paths["wheel_evidence"],
            {
                "kind": "postgamma.python-wheel-evidence",
                "status": "pass",
                "wheel": {"filename": wheel.name, "sha256": wheel_digest},
                "gates": {"integration": {"scenario_count": 16}},
            },
        )
        write_json(
            paths["docs_evidence"],
            {
                "kind": "postgamma.documentation-evidence",
                "status": "pass",
                "markdown_file_count": 38,
            },
        )
        write_json(
            paths["publication_evidence"],
            {
                "kind": "postgamma.public-surface-evidence",
                "status": "pass",
            },
        )
        write_json(
            paths["artifact_publication_evidence"],
            {
                "kind": "postgamma.public-artifact-evidence",
                "status": "pass",
                "build_path_violations": 0,
                "static_sdk": {"sha256": static_digest},
                "python_wheel": {"sha256": wheel_digest},
            },
        )
        return paths

    def close(self, root: Path, paths: dict[str, Path]) -> dict[str, object]:
        return check_release_candidate.close_candidate(
            root=root,
            version="0.1.0a1",
            python_release_path=paths["python_release"],
            conformance_path=paths["conformance"],
            static_evidence_path=paths["static_evidence"],
            static_receipt_path=paths["static_receipt"],
            static_archive_path=paths["static_archive"],
            static_checksum_path=paths["static_checksum"],
            wheel_receipt_path=paths["wheel_receipt"],
            wheel_evidence_path=paths["wheel_evidence"],
            wheel_directory=paths["wheel_directory"],
            docs_evidence_path=paths["docs_evidence"],
            publication_evidence_path=paths["publication_evidence"],
            artifact_publication_evidence_path=paths[
                "artifact_publication_evidence"
            ],
            docs_site=paths["docs_site"],
        )

    def test_accepts_complete_tested_artifact_set(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            report = self.close(root, self.fixture(root))
            self.assertEqual(report["status"], "pass")
            self.assertTrue(report["artifacts"]["static_sdk"]["tested"])
            self.assertEqual(report["embedded_conformance"]["cases"], 54)
            self.assertIn("public_surface_validation", report["evidence"])
            self.assertIn("public_artifact_validation", report["evidence"])

    def test_rejects_archive_changed_after_testing(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            paths = self.fixture(root)
            paths["static_archive"].write_bytes(b"changed")
            with self.assertRaisesRegex(
                check_release_candidate.ReleaseCandidateError,
                "archive or checksum",
            ):
                self.close(root, paths)

    def test_rejects_static_library_not_covered_by_test_evidence(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            paths = self.fixture(root)
            receipt = json.loads(paths["static_receipt"].read_text(encoding="utf-8"))
            receipt["static_library_sha256"] = hashlib.sha256(b"other").hexdigest()
            write_json(paths["static_receipt"], receipt)
            with self.assertRaisesRegex(
                check_release_candidate.ReleaseCandidateError,
                "tested library",
            ):
                self.close(root, paths)

    def test_release_target_pins_one_postgresql_configure_profile(self) -> None:
        makefile = (ROOT / "Makefile").read_text(encoding="utf-8")
        self.assertIn(
            "release-candidate: override PG_CONFIGURE_ARGS := $(strip \\\n"
            "\t$(POSTGAMMA_FULL_TEST_BASE_PG_CONFIGURE_ARGS) \\\n"
            "\t$(POSTGAMMA_FULL_TEST_REQUIRED_PG_CONFIGURE_ARGS))",
            makefile,
        )


if __name__ == "__main__":
    unittest.main()
