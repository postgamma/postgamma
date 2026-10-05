"""Exercise source change detection and reuse of actual CI test outcomes."""

from __future__ import annotations

import os
import subprocess
import sys
import tempfile
import unittest
import urllib.error
from pathlib import Path
from unittest.mock import Mock, patch


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "buildsys"))

from plan_upstream_ci import should_run, source_inputs, test_identity  # noqa: E402


INPUTS = {
    "postgamma_commit": "a" * 40,
    "postgres_commit": "b" * 40,
    "postgres_url": "https://example.invalid/postgres.git",
    "pgvector_url": "https://example.invalid/pgvector.git",
    "branch": "master",
    "profile": "weekly",
}


def job(identifier: int, conclusion: str = "success", *,
        step_conclusion: str | None = None, inputs: dict[str, str] = INPUTS) -> dict:
    return {
        "id": identifier,
        "head_sha": inputs["postgamma_commit"],
        "status": "completed",
        "conclusion": conclusion,
        "completed_at": f"2026-01-01T00:{identifier:02d}:00Z",
        "steps": [{
            "name": f"Test {test_identity(inputs)}",
            "conclusion": step_conclusion or conclusion,
        }],
    }


def history(*batches: list[dict]) -> Mock:
    runs = [
        {"id": index, "head_sha": INPUTS["postgamma_commit"]}
        for index in range(len(batches))
    ]

    def get(path: str, **parameters: object) -> dict:
        if path.endswith("/runs"):
            return {"workflow_runs": runs}
        index = int(path.split("/")[2])
        page = int(parameters["page"])
        return {"jobs": batches[index][(page - 1) * 100:page * 100]}

    return Mock(get=Mock(side_effect=get))


class UpstreamCIHistoryTests(unittest.TestCase):
    def planned(self, api: Mock, inputs: dict[str, str] = INPUTS,
                event: str = "schedule", attempt: int = 1) -> bool:
        return should_run(api, inputs, event, "current", attempt)[0]

    def test_unchanged_success_skips_and_missing_result_runs(self) -> None:
        self.assertFalse(self.planned(history([job(1)])))
        self.assertTrue(self.planned(history()))

    def test_each_changed_input_invalidates_previous_success(self) -> None:
        for name in INPUTS:
            with self.subTest(input=name):
                changed = dict(INPUTS, **{name: INPUTS[name] + "changed"})
                self.assertTrue(self.planned(history([job(1)]), changed))

    def test_profiles_and_postgres_branches_are_independent(self) -> None:
        daily = dict(INPUTS, profile="daily")
        stable = dict(INPUTS, branch="REL_19_STABLE")
        api = history([job(1, inputs=daily), job(2, inputs=stable)])
        self.assertTrue(self.planned(api))
        self.assertFalse(self.planned(api, daily))
        self.assertFalse(self.planned(api, stable))

    def test_newer_failure_overrides_success_and_retry_success_recovers(self) -> None:
        for conclusion in ("failure", "cancelled", "timed_out"):
            with self.subTest(conclusion=conclusion):
                self.assertTrue(self.planned(history([job(1)], [job(2, conclusion)])))
                self.assertFalse(self.planned(history([job(3)], [job(2, conclusion)])))

    def test_skipped_tests_do_not_create_or_replace_a_result(self) -> None:
        skipped = job(3, step_conclusion="skipped")
        self.assertTrue(self.planned(history([skipped])))
        self.assertFalse(self.planned(history([skipped], [job(1)])))
        self.assertTrue(self.planned(history([skipped], [job(2, "failure")])))

    def test_successful_sibling_does_not_hide_a_failure(self) -> None:
        stable = dict(INPUTS, branch="REL_19_STABLE")
        api = history([job(1), job(2, "failure", inputs=stable)])
        self.assertFalse(self.planned(api))
        self.assertTrue(self.planned(api, stable))

    def test_failed_setup_or_artifact_upload_cannot_record_success(self) -> None:
        for step_conclusion in ("skipped", "success"):
            with self.subTest(step_conclusion=step_conclusion):
                self.assertTrue(self.planned(history([
                    job(1), job(2, "failure", step_conclusion=step_conclusion)
                ])))

    def test_retried_old_run_uses_latest_completion_including_job_pages(self) -> None:
        unrelated = dict(INPUTS, branch="REL_19_STABLE")
        old_attempts = [job(1, inputs=unrelated)] * 100
        old_attempts.append(job(3, "failure"))
        self.assertTrue(self.planned(history([job(2)], old_attempts)))

    def test_manual_dispatch_and_retries_always_run_without_history_lookup(self) -> None:
        api = history([job(1)])
        self.assertTrue(self.planned(api, event="workflow_dispatch"))
        self.assertTrue(self.planned(api, attempt=2))
        api.get.assert_not_called()

    def test_unavailable_or_malformed_history_runs_tests(self) -> None:
        api = Mock()
        for error in (urllib.error.URLError("unavailable"), ValueError(), KeyError()):
            with self.subTest(error=type(error).__name__):
                api.get.side_effect = error
                self.assertTrue(self.planned(api))


class UpstreamCISourceTests(unittest.TestCase):
    def setUp(self) -> None:
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.directory = Path(temporary.name)
        self.root = self.directory / "project"
        self.postgres = self.directory / "postgres"
        for path in (self.root, self.postgres):
            self.git(self.directory, "init", "--quiet", "--initial-branch=master", str(path))
            self.git(path, "config", "user.name", "CI test")
            self.git(path, "config", "user.email", "ci@example.invalid")
        self.git(self.postgres, "commit", "--quiet", "--allow-empty", "-m", "Initial")
        self.git(self.postgres, "branch", "REL_19_STABLE")
        self.git(self.root, "config", "-f", ".gitmodules", "submodule.postgres.url", str(self.postgres))
        self.git(self.root, "config", "-f", ".gitmodules", "submodule.third_party/pgvector.url",
                 "https://example.invalid/pgvector.git")
        self.git(self.root, "add", ".gitmodules")
        self.git(self.root, "commit", "--quiet", "-m", "Initial")
        environment = patch.dict(os.environ, PG_REPOSITORY="", PGVECTOR_REPOSITORY="")
        environment.start()
        self.addCleanup(environment.stop)

    def git(self, root: Path, *arguments: str) -> str:
        return subprocess.check_output(
            ["git", "-C", str(root), *arguments], text=True, stderr=subprocess.STDOUT
        ).strip()

    def test_upstream_update_changes_only_the_relevant_branch_identity(self) -> None:
        before = {branch: source_inputs(self.root, "weekly", branch)
                  for branch in ("", "master", "REL_19_STABLE")}
        pinned = self.git(self.postgres, "rev-parse", "HEAD")
        self.assertEqual(before["master"]["postgres_commit"], pinned)
        self.git(self.postgres, "commit", "--quiet", "--allow-empty", "-m", "Advance master")
        for branch in before:
            with self.subTest(branch=branch):
                after = source_inputs(self.root, "weekly", branch)
                self.assertEqual(before[branch] == after, branch != "master")
        self.assertEqual(before["master"]["postgres_commit"], pinned)
        self.assertFalse((self.root / "postgres").exists())

    def test_configured_mirror_resolves_the_same_commit(self) -> None:
        before = source_inputs(self.root, "weekly", "master")
        mirror = self.directory / "mirror"
        self.git(self.directory, "clone", "--quiet", "--bare", str(self.postgres), str(mirror))
        with patch.dict(os.environ, PG_REPOSITORY=str(mirror)):
            after = source_inputs(self.root, "weekly", "master")
        self.assertEqual(before["postgres_commit"], after["postgres_commit"])
        self.assertNotEqual(test_identity(before), test_identity(after))

    def test_unknown_branch_does_not_reuse_another_commit(self) -> None:
        with self.assertRaises(subprocess.CalledProcessError):
            source_inputs(self.root, "weekly", "nonexistent")


if __name__ == "__main__":
    unittest.main()
