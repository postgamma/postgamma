"""Tests for the embedded-bootstrap executable evidence contract."""

from __future__ import annotations

import json
import sys
import tempfile
import unittest
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(PROJECT_ROOT / "buildsys"))

import check_bootstrap_evidence  # noqa: E402


class BootstrapEvidenceTests(unittest.TestCase):
    profile_path = PROJECT_ROOT / "manifests/profiles/embedded-bootstrap.json"
    upstream_path = PROJECT_ROOT / "manifests/upstream.json"
    adapter_path = PROJECT_ROOT / "manifests/postgresql/adapter.json"
    embedded_adapter_path = PROJECT_ROOT / "manifests/postgresql/embedded.json"

    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.configure_state = Path(self.temporary.name) / "configure-state.json"
        self.config_log = Path(self.temporary.name) / "config.log"
        self.config_log.write_text(
            "compiler: fixture-cc 1.0\n", encoding="utf-8"
        )
        self.write(
            self.configure_state,
            {
                "schema_version": 1,
                "source": "/portable/postgres",
                "configure_sha256": "1" * 64,
                "arguments": ["--enable-debug"],
                "environment": {"CC": "cc", "CFLAGS": "-O2"},
            },
        )

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def identity(self) -> tuple[dict[str, object], dict[str, object]]:
        return check_bootstrap_evidence.baseline_identity(
            self.upstream_path,
            self.adapter_path,
            self.embedded_adapter_path,
            self.profile_path,
            self.configure_state,
            self.config_log,
            "embedded-bootstrap",
        )

    def leaf(
        self,
        claim: str,
        baseline: dict[str, object],
        status: str = "pass",
        artifact_root: Path | None = None,
    ) -> dict[str, object]:
        artifacts: list[dict[str, str]] = []
        if artifact_root is not None:
            artifact = artifact_root / f"artifacts/{claim}.log"
            artifact.parent.mkdir(parents=True, exist_ok=True)
            artifact.write_text(f"{claim}\n", encoding="utf-8")
            artifacts.append(
                {
                    "path": artifact.relative_to(artifact_root).as_posix(),
                    "sha256": check_bootstrap_evidence.sha256(artifact),
                }
            )
        return {
            "schema_version": 1,
            "kind": check_bootstrap_evidence.LEAF_KIND,
            "claim": claim,
            "status": status,
            "baseline": baseline.copy(),
            "command": ["test-probe", claim],
            "artifacts": artifacts,
            "limitations": [],
            "metrics": {"checks": 1},
        }

    @staticmethod
    def support(baseline: dict[str, object]) -> dict[str, object]:
        return {
            "schema_version": 1,
            "kind": "postgamma.embedded-process-assumptions",
            "status": "pass",
            "baseline": baseline.copy(),
            "product_ready": False,
            "embedded_kernel_surface_frozen": True,
            "records": [{"id": "fixture", "status": "classified"}],
        }

    @staticmethod
    def write(path: Path, document: dict[str, object]) -> None:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(document, sort_keys=True) + "\n", encoding="utf-8")

    def test_initial_report_is_incomplete_and_never_product_ready(self) -> None:
        baseline, profile = self.identity()
        with tempfile.TemporaryDirectory() as temporary:
            report = check_bootstrap_evidence.collect_report(
                profile, baseline, Path(temporary)
            )
        self.assertFalse(report["bootstrap_complete"])
        self.assertFalse(report["product_ready"])
        self.assertEqual(set(report["claims"].values()), {"not_run"})

    def test_completion_requires_exactly_the_required_leaf_claims(self) -> None:
        baseline, profile = self.identity()
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            evidence = root / "reports"
            evidence.mkdir()
            for claim in profile["claims"]:
                if claim["required_for_completion"]:
                    self.write(
                        evidence / claim["evidence"],
                        self.leaf(claim["id"], baseline, artifact_root=root),
                    )
            for support in profile["supporting_evidence"]:
                self.write(
                    evidence / support["evidence"], self.support(baseline)
                )
            report = check_bootstrap_evidence.collect_report(profile, baseline, evidence)
        self.assertTrue(report["bootstrap_complete"])
        self.assertFalse(report["product_ready"])
        self.assertEqual(report["claims"]["host_safe_lifecycle"], "not_run")
        self.assertEqual(report["claims"]["complete_initdb"], "not_run")
        self.assertEqual(report["claims"]["real_embedded_query"], "not_run")
        self.assertEqual(
            report["supporting_evidence_status"], {"process_assumptions": "pass"}
        )

    def test_completion_fails_closed_without_supporting_evidence(self) -> None:
        baseline, profile = self.identity()
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            evidence = root / "reports"
            evidence.mkdir()
            for claim in profile["claims"]:
                if claim["required_for_completion"]:
                    self.write(
                        evidence / claim["evidence"],
                        self.leaf(claim["id"], baseline, artifact_root=root),
                    )
            report = check_bootstrap_evidence.collect_report(profile, baseline, evidence)
        self.assertFalse(report["bootstrap_complete"])
        self.assertEqual(
            report["supporting_evidence_status"],
            {"process_assumptions": "not_run"},
        )

    def test_leaf_baseline_must_match_the_current_build(self) -> None:
        baseline, profile = self.identity()
        claim = profile["claims"][0]
        leaf = self.leaf(claim["id"], baseline)
        leaf["baseline"]["upstream_commit"] = "0" * 40
        with self.assertRaisesRegex(
            check_bootstrap_evidence.BootstrapEvidenceError, "current build"
        ):
            check_bootstrap_evidence.validate_leaf(
                leaf, claim["id"], baseline, Path(self.temporary.name), "fixture"
            )

    def test_leaf_build_profile_must_match_the_current_build(self) -> None:
        baseline, profile = self.identity()
        claim = profile["claims"][0]
        leaf = self.leaf(claim["id"], baseline)
        leaf["baseline"]["build_profile"] = "another-profile"
        with self.assertRaisesRegex(
            check_bootstrap_evidence.BootstrapEvidenceError, "current build"
        ):
            check_bootstrap_evidence.validate_leaf(
                leaf, claim["id"], baseline, Path(self.temporary.name), "fixture"
            )

    def test_configure_state_hash_is_part_of_the_baseline(self) -> None:
        first, _profile = self.identity()
        state = check_bootstrap_evidence.load_json(self.configure_state)
        state["environment"]["CFLAGS"] = "-O0"
        self.write(self.configure_state, state)
        second, _profile = self.identity()
        self.assertNotEqual(
            first["configure_state_sha256"], second["configure_state_sha256"]
        )

    def test_toolchain_log_hash_is_part_of_the_baseline(self) -> None:
        first, _profile = self.identity()
        self.config_log.write_text(
            "compiler: fixture-cc 2.0\n", encoding="utf-8"
        )
        second, _profile = self.identity()
        self.assertNotEqual(
            first["toolchain_config_log_sha256"],
            second["toolchain_config_log_sha256"],
        )

    def test_leaf_artifact_hash_must_match_the_current_build_product(self) -> None:
        baseline, profile = self.identity()
        root = Path(self.temporary.name)
        artifact = root / "lib/probe.so"
        artifact.parent.mkdir()
        artifact.write_bytes(b"first")
        leaf = self.leaf(profile["claims"][0]["id"], baseline)
        leaf["artifacts"] = [
            {
                "path": "lib/probe.so",
                "sha256": check_bootstrap_evidence.sha256(artifact),
            }
        ]
        check_bootstrap_evidence.validate_leaf(
            leaf, profile["claims"][0]["id"], baseline, root, "fixture"
        )
        artifact.write_bytes(b"changed")
        with self.assertRaisesRegex(
            check_bootstrap_evidence.BootstrapEvidenceError, "missing or stale"
        ):
            check_bootstrap_evidence.validate_leaf(
                leaf, profile["claims"][0]["id"], baseline, root, "fixture"
            )

    def test_pass_leaf_requires_at_least_one_hashed_artifact(self) -> None:
        baseline, profile = self.identity()
        leaf = self.leaf(profile["claims"][0]["id"], baseline)
        with self.assertRaisesRegex(
            check_bootstrap_evidence.BootstrapEvidenceError, "must not be empty"
        ):
            check_bootstrap_evidence.validate_leaf(
                leaf,
                profile["claims"][0]["id"],
                baseline,
                Path(self.temporary.name),
                "fixture",
            )

    def test_verification_rejects_a_stale_leaf_hash(self) -> None:
        baseline, profile = self.identity()
        claim = profile["claims"][0]
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            evidence = root / "reports"
            evidence.mkdir()
            leaf_path = evidence / claim["evidence"]
            self.write(
                leaf_path,
                self.leaf(claim["id"], baseline, artifact_root=root),
            )
            report = check_bootstrap_evidence.collect_report(profile, baseline, evidence)
            changed = self.leaf(claim["id"], baseline, artifact_root=root)
            changed["metrics"] = {"checks": 2}
            self.write(leaf_path, changed)
            with self.assertRaisesRegex(
                check_bootstrap_evidence.BootstrapEvidenceError, "stale leaf evidence hash"
            ):
                check_bootstrap_evidence.validate_report(
                    report, profile, baseline, evidence
                )

    def test_verification_rejects_product_ready(self) -> None:
        baseline, profile = self.identity()
        with tempfile.TemporaryDirectory() as temporary:
            evidence = Path(temporary)
            report = check_bootstrap_evidence.collect_report(profile, baseline, evidence)
            report["product_ready"] = True
            with self.assertRaisesRegex(
                check_bootstrap_evidence.BootstrapEvidenceError, "product_ready"
            ):
                check_bootstrap_evidence.validate_report(
                    report, profile, baseline, evidence
                )

    def test_strict_json_loader_rejects_duplicate_keys(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "duplicate.json"
            path.write_text('{"schema_version": 1, "schema_version": 1}\n')
            with self.assertRaisesRegex(
                check_bootstrap_evidence.BootstrapEvidenceError, "duplicate JSON key"
            ):
                check_bootstrap_evidence.load_json(path)

    def test_profile_rejects_unknown_fields(self) -> None:
        profile = check_bootstrap_evidence.load_json(self.profile_path)
        profile["typo"] = True
        with self.assertRaisesRegex(
            check_bootstrap_evidence.BootstrapEvidenceError, "unknown field"
        ):
            check_bootstrap_evidence.validate_profile(profile)


if __name__ == "__main__":
    unittest.main()
