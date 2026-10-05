"""Tests for least-authority public release workflows."""

from __future__ import annotations

import os
import subprocess
import sys
import tempfile
import unittest
from fnmatch import fnmatchcase
from pathlib import Path
from unittest import mock

import yaml

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "buildsys"))

import check_python_release_workflow  # noqa: E402
from workflow_contract import read_workflow, shell_commands  # noqa: E402


class ReleaseWorkflowTests(unittest.TestCase):
    def test_missing_yaml_dependency_has_an_installation_diagnostic(self) -> None:
        with mock.patch.dict(sys.modules, {"yaml": None}):
            with self.assertRaisesRegex(
                check_python_release_workflow.WorkflowContractError,
                "install buildsys/requirements.txt",
            ):
                self.workflow()

    def test_actions_on_key_does_not_change_the_global_yaml_loader(self) -> None:
        before = yaml.safe_load("on: true\n")
        document = self.workflow()
        self.assertIn("on", document)
        self.assertEqual(yaml.safe_load("on: true\n"), before)

    def workflow(self) -> dict:
        return read_workflow(ROOT / ".github/workflows/python-wheels.yml")

    def check_document(self, document: dict) -> dict:
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "workflow.yml"
            path.write_text(yaml.safe_dump(document, sort_keys=False), encoding="utf-8")
            return check_python_release_workflow.check(
                path,
                ROOT / "manifests/api/python-api-v1.json",
                ROOT / ".github/workflows/pypi.yml",
                ROOT / "buildsys/toolchains/llvm22-linux-64.lock",
            )

    def check_pypi_document(self, document: dict) -> dict:
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "pypi.yml"
            path.write_text(yaml.safe_dump(document, sort_keys=False), encoding="utf-8")
            return check_python_release_workflow.validate_pypi_workflow(path)

    def run_step(self, document: dict, job: str, command: str) -> dict:
        return next(
            step
            for step in document["jobs"][job]["steps"]
            if command in step.get("run", "")
        )

    def test_repository_workflows_use_exact_tested_bytes(self) -> None:
        report = check_python_release_workflow.check(
            ROOT / ".github/workflows/python-wheels.yml",
            ROOT / "manifests/api/python-api-v1.json",
            ROOT / ".github/workflows/pypi.yml",
            ROOT / "buildsys/toolchains/llvm22-linux-64.lock",
        )
        self.assertTrue(report["exact_tested_byte_promotion"])
        self.assertTrue(report["draft_before_approval"])
        self.assertTrue(report["failed_wheel_diagnostics_preserved"])
        self.assertTrue(report["explicit_pypi_dispatch_after_publication"])
        self.assertEqual(report["glibc_floor"], "2.17")
        self.assertEqual(report["cpu_baseline"], "x86-64-v1")
        self.assertEqual(report["toolchain"]["llvm_version"], "22.1.8")
        self.assertTrue(report["pypi"]["trusted_publishing"])
        self.assertFalse(report["pypi"]["stored_upload_token"])
        self.assertTrue(report["pypi"]["published_tag_required"])

    def test_pypi_dispatch_cannot_be_dropped_or_changed_to_another_ref(self) -> None:
        for change in ("missing", "wrong_workflow", "wrong_ref", "missing_permission"):
            with self.subTest(change=change):
                document = self.workflow()
                job = document["jobs"]["dispatch-pypi"]
                if change == "missing":
                    del document["jobs"]["dispatch-pypi"]
                elif change == "missing_permission":
                    del job["permissions"]["actions"]
                else:
                    step = self.run_step(document, "dispatch-pypi", "gh workflow run")
                    before, after = (
                        ("pypi.yml", "python-wheels.yml")
                        if change == "wrong_workflow"
                        else ('--ref "$RELEASE_TAG"', "--ref main")
                    )
                    step["run"] = step["run"].replace(before, after)
                with self.assertRaises(
                    check_python_release_workflow.WorkflowContractError
                ):
                    self.check_document(document)

    def test_pypi_dispatch_keeps_the_owner_switch_and_version_output(self) -> None:
        for change in ("owner_switch", "tag"):
            with self.subTest(change=change):
                document = self.workflow()
                job = document["jobs"]["dispatch-pypi"]
                if change == "owner_switch":
                    del job["if"]
                else:
                    job["steps"][0]["env"]["RELEASE_TAG"] = "${{ github.ref_name }}"
                with self.assertRaises(
                    check_python_release_workflow.WorkflowContractError
                ):
                    self.check_document(document)

    def test_pypi_requires_dispatch_and_rejects_branch_publication(self) -> None:
        for change in ("dispatch", "tag_guard", "checkout", "tag", "override"):
            with self.subTest(change=change):
                document = read_workflow(ROOT / ".github/workflows/pypi.yml")
                job = document["jobs"]["publish"]
                if change == "dispatch":
                    del document["on"]["workflow_dispatch"]
                elif change == "tag_guard":
                    job["if"] = "vars.POSTGAMMA_PYPI_PUBLISH_ENABLED == 'true'"
                elif change == "checkout":
                    job["steps"][0]["with"]["ref"] = "main"
                elif change == "tag":
                    job["env"]["RELEASE_TAG"] = "v0.1.0a1"
                else:
                    job["steps"][1]["env"] = {"RELEASE_TAG": "v0.1.0a1"}
                with self.assertRaises(
                    check_python_release_workflow.WorkflowContractError
                ):
                    self.check_pypi_document(document)

    def test_pypi_publication_and_asset_gates_cannot_be_bypassed(self) -> None:
        replacements = (
            ('test "$is_draft" = false', 'echo test "$is_draft" = false'),
            ('test "$is_draft" = false', 'test "$is_draft" = true'),
            ('--tag "$RELEASE_TAG"', '--tag "v0.1.0a2"'),
            ('gh release download "$RELEASE_TAG"', 'gh release download latest'),
            ('--pattern "SHA256SUMS"', '--pattern "unrelated.txt"'),
            ('--bundle dist', '--bundle unverified'),
        )
        for before, after in replacements:
            with self.subTest(after=after):
                document = read_workflow(ROOT / ".github/workflows/pypi.yml")
                for step in document["jobs"]["publish"]["steps"]:
                    if "run" in step:
                        step["run"] = step["run"].replace(before, after)
                with self.assertRaises(
                    check_python_release_workflow.WorkflowContractError
                ):
                    self.check_pypi_document(document)
        document = read_workflow(ROOT / ".github/workflows/pypi.yml")
        job = document["jobs"]["publish"]
        gate = self.run_step(document, "publish", "is_draft=")
        job["steps"].remove(gate)
        job["steps"].insert(-1, gate)
        with self.assertRaisesRegex(
            check_python_release_workflow.WorkflowContractError,
            "validate publication before downloading",
        ):
            self.check_pypi_document(document)

    def run_with_fake_gh(
        self, script: str, *, output: str = "", status: int = 0
    ) -> tuple[subprocess.CompletedProcess[str], list[str]]:
        """Run the workflow shell with a local CLI double, without any network call."""
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            gh = root / "gh"
            gh.write_text(
                '#!/bin/bash\n'
                'printf "%s\\n" "$@" >> "$CLI_TRACE"\n'
                'printf "%s\\n" "$CLI_OUTPUT"\n'
                'exit "$CLI_STATUS"\n'
            )
            gh.chmod(0o755)
            result = subprocess.run(
                ["bash", "--noprofile", "--norc", "-eo", "pipefail", "-c", script],
                cwd=root,
                env={
                    **os.environ,
                    "PATH": f"{root}:{os.environ['PATH']}",
                    "GITHUB_REPOSITORY": "postgamma/postgamma",
                    "RELEASE_TAG": "v0.1.0a1",
                    "CLI_TRACE": str(root / "calls"),
                    "CLI_OUTPUT": output,
                    "CLI_STATUS": str(status),
                },
                text=True,
                capture_output=True,
                timeout=10,
                check=False,
            )
            return result, (root / "calls").read_text().splitlines()

    def test_dispatch_shell_uses_the_published_tag_and_propagates_failure(self) -> None:
        step = self.run_step(self.workflow(), "dispatch-pypi", "gh workflow run")
        for status in (0, 1):
            with self.subTest(status=status):
                result, calls = self.run_with_fake_gh(step["run"], status=status)
                self.assertEqual(result.returncode, status, result.stderr)
                self.assertEqual(
                    calls,
                    [
                        "workflow", "run", "pypi.yml", "--repo", "postgamma/postgamma",
                        "--ref", "v0.1.0a1",
                    ],
                )

    def test_publication_shell_requires_a_published_release(self) -> None:
        document = read_workflow(ROOT / ".github/workflows/pypi.yml")
        step = self.run_step(document, "publish", "is_draft=")
        for output, status, expected_success in (
            ("false", 0, True),
            ("true", 0, False),
            ("null", 0, False),
            ("", 1, False),
            ("false", 1, False),
        ):
            with self.subTest(output=output, status=status):
                result, calls = self.run_with_fake_gh(
                    step["run"], output=output, status=status
                )
                self.assertEqual(
                    result.returncode == 0, expected_success, result.stderr
                )
                self.assertEqual(
                    calls,
                    [
                        "release", "view", "v0.1.0a1", "--repo", "postgamma/postgamma",
                        "--json", "isDraft", "--jq", ".isDraft",
                    ],
                )

    def test_pypi_workflow_rejects_a_stored_token(self) -> None:
        document = read_workflow(ROOT / ".github/workflows/pypi.yml")
        document["jobs"]["publish"]["env"]["TWINE_PASSWORD"] = "${{ secrets.PYPI_TOKEN }}"
        with self.assertRaisesRegex(
            check_python_release_workflow.WorkflowContractError,
            "stored upload token",
        ):
            self.check_pypi_document(document)

    def test_pypi_workflow_rejects_credentials_configured_in_run_steps(self) -> None:
        content = (ROOT / ".github/workflows/pypi.yml").read_text()
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "pypi.yml"
            content = content.replace(
                "          mkdir -p dist release-metadata",
                "          export TWINE_PASSWORD='${{ secrets.PYPI_TOKEN }}'\n"
                "          mkdir -p dist release-metadata",
            )
            path.write_text(content)
            with self.assertRaisesRegex(
                check_python_release_workflow.WorkflowContractError,
                "stored upload token",
            ):
                check_python_release_workflow.validate_pypi_workflow(path)

    def test_comments_do_not_supply_or_change_workflow_configuration(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "pypi.yml"
            content = (ROOT / ".github/workflows/pypi.yml").read_text()
            path.write_text(
                content + "\n# TWINE_PASSWORD is not used.\n# id-token: write\n"
            )
            self.assertTrue(
                check_python_release_workflow.validate_pypi_workflow(path)[
                    "trusted_publishing"
                ]
            )
            content = content.replace(
                "      id-token: write\n", "      # id-token: write\n"
            )
            path.write_text(content)
            with self.assertRaisesRegex(
                check_python_release_workflow.WorkflowContractError, "permissions"
            ):
                check_python_release_workflow.validate_pypi_workflow(path)

    def test_equivalent_yaml_and_shell_argument_spelling_passes(self) -> None:
        document = self.workflow()
        document["env"]["SOURCE_DATE_EPOCH"] = 315532800
        document["jobs"]["kernel"]["needs"] = ["version"]
        document["jobs"]["wheels"]["strategy"]["matrix"]["python_tag"].reverse()
        document["jobs"]["release-gate"]["needs"].reverse()
        document["jobs"]["publish-github-release"]["environment"] = {
            "name": "github-release"
        }
        step = self.run_step(document, "wheels", "buildsys/run_python_release.py")
        step["run"] = step["run"].replace(
            '--platform "$MANYLINUX_POLICY"', '--platform="$MANYLINUX_POLICY"'
        )
        self.assertEqual(
            self.check_document(document)["python_tags"],
            check_python_release_workflow.EXPECTED_PYTHON_TAGS,
        )
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "flow.yml"
            path.write_text(yaml.safe_dump(document, default_flow_style=True))
            result = check_python_release_workflow.check(
                path,
                ROOT / "manifests/api/python-api-v1.json",
                ROOT / ".github/workflows/pypi.yml",
                ROOT / "buildsys/toolchains/llvm22-linux-64.lock",
            )
            self.assertEqual(result["status"], "pass")

    def test_echo_and_comments_cannot_replace_an_executed_check(self) -> None:
        for replacement in ("echo make docs-check", "# make docs-check"):
            with self.subTest(replacement=replacement):
                document = self.workflow()
                step = self.run_step(document, "documentation", "make docs-check")
                step["run"] = step["run"].replace("make docs-check", replacement)
                with self.assertRaisesRegex(
                    check_python_release_workflow.WorkflowContractError,
                    "strict release documentation",
                ):
                    self.check_document(document)

    def test_data_heredoc_cannot_supply_commands_and_checker_never_executes_shell(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            marker = Path(temporary) / "must-not-exist"
            script = f"cat <<'DATA'\nmake docs-check\nDATA\ntouch '{marker}'\n"
            commands = shell_commands(script)
            self.assertFalse(marker.exists())
            self.assertEqual(commands, [["cat"], ["touch", str(marker)]])
        document = self.workflow()
        step = self.run_step(document, "documentation", "make docs-check")
        step["run"] = "cat <<'DATA'\n" + step["run"] + "DATA\n"
        with self.assertRaisesRegex(
            check_python_release_workflow.WorkflowContractError,
            "strict release documentation",
        ):
            self.check_document(document)

    def test_release_dependencies_cannot_omit_a_required_gate(self) -> None:
        for job, dependency in (
            ("wheels", "kernel"),
            ("release-gate", "wheels"),
            ("release-gate", "documentation"),
            ("stage-github-release", "release-gate"),
            ("publish-github-release", "stage-github-release"),
            ("dispatch-pypi", "publish-github-release"),
        ):
            with self.subTest(job=job, dependency=dependency):
                document = self.workflow()
                document["jobs"][job]["needs"].remove(dependency)
                with self.assertRaisesRegex(
                    check_python_release_workflow.WorkflowContractError, "dependencies"
                ):
                    self.check_document(document)

    def test_write_permissions_are_bound_to_the_publication_jobs(self) -> None:
        document = self.workflow()
        document["jobs"]["kernel"]["permissions"] = {"contents": "write"}
        del document["jobs"]["stage-github-release"]["permissions"]
        with self.assertRaisesRegex(
            check_python_release_workflow.WorkflowContractError, "kernel.*permissions"
        ):
            self.check_document(document)
        document = self.workflow()
        document["jobs"]["kernel"]["permissions"] = {
            "contents": "read", "actions": "write"
        }
        with self.assertRaisesRegex(
            check_python_release_workflow.WorkflowContractError, "kernel.*permissions"
        ):
            self.check_document(document)

    def test_matrix_exclusions_missing_versions_and_extra_axes_are_rejected(
        self,
    ) -> None:
        for change in ("missing", "exclude", "extra_axis"):
            with self.subTest(change=change):
                document = self.workflow()
                matrix = document["jobs"]["wheels"]["strategy"]["matrix"]
                if change == "missing":
                    matrix["python_tag"].pop()
                elif change == "exclude":
                    matrix["exclude"] = [{"python_tag": "cp310"}]
                else:
                    matrix["os"] = ["ubuntu-24.04", "ubuntu-22.04"]
                with self.assertRaisesRegex(
                    check_python_release_workflow.WorkflowContractError,
                    "release matrix",
                ):
                    self.check_document(document)

    def test_required_jobs_and_steps_cannot_be_skipped_or_ignore_failure(self) -> None:
        for field, value in (("if", False), ("continue-on-error", True)):
            for scope in ("job", "step"):
                with self.subTest(field=field, scope=scope):
                    document = self.workflow()
                    target = (
                        document["jobs"]["wheels"]
                        if scope == "job"
                        else self.run_step(document, "wheels", "docker run")
                    )
                    target[field] = value
                    with self.assertRaises(
                        check_python_release_workflow.WorkflowContractError
                    ):
                        self.check_document(document)

    def test_failed_wheel_logs_are_archived_separately_from_release_inputs(self) -> None:
        document = self.workflow()
        diagnostics = next(
            step for step in document["jobs"]["wheels"]["steps"]
            if step.get("with", {}).get("name")
            == check_python_release_workflow.WHEEL_DIAGNOSTICS_ARTIFACT
        )
        pattern = next(
            step["with"]["pattern"]
            for step in document["jobs"]["release-gate"]["steps"]
            if "pattern" in step.get("with", {})
        )
        self.assertFalse(fnmatchcase(diagnostics["with"]["name"], pattern))
        self.assertEqual(diagnostics["if"], "failure()")
        for change in ("missing", "success_only", "stderr_missing", "ignore_failure"):
            with self.subTest(change=change):
                changed = self.workflow()
                job = changed["jobs"]["wheels"]
                step = next(
                    item for item in job["steps"]
                    if item.get("with", {}).get("name") == diagnostics["with"]["name"]
                )
                if change == "missing":
                    job["steps"].remove(step)
                elif change == "success_only":
                    del step["if"]
                elif change == "stderr_missing":
                    step["with"]["path"] = step["with"]["path"].replace(
                        "build/python-release/${{ matrix.python_tag }}/check/*.stderr\n",
                        "",
                    )
                else:
                    step["continue-on-error"] = True
                with self.assertRaises(
                    check_python_release_workflow.WorkflowContractError
                ):
                    self.check_document(changed)

    def test_job_local_environment_cannot_weaken_the_build_baseline(self) -> None:
        document = self.workflow()
        document["jobs"]["wheels"]["env"] = {"RELEASE_CFLAGS": "-O2 -march=native"}
        with self.assertRaisesRegex(
            check_python_release_workflow.WorkflowContractError,
            "overrides generic x86-64 flags",
        ):
            self.check_document(document)

    def test_commented_platform_gate_is_not_an_execution_argument(self) -> None:
        document = self.workflow()
        step = self.run_step(document, "wheels", "--platform")
        step["run"] = step["run"].replace('--platform "$MANYLINUX_POLICY"', "")
        step["run"] += '\n# --platform "$MANYLINUX_POLICY"\n'
        with self.assertRaisesRegex(
            check_python_release_workflow.WorkflowContractError, "--platform must be"
        ):
            self.check_document(document)

    def test_shared_runner_cannot_be_replaced_by_echo_or_use_other_inputs(self) -> None:
        for before, after in (
            (
                '"$python_executable" buildsys/run_python_release.py',
                "echo buildsys/run_python_release.py",
            ),
            (
                "--static-library build/python-manylinux-kernel/libpostgamma_python_abi1.a",
                "--static-library build/another-kernel.a",
            ),
            ('--output-dir "$release_root"', "--output-dir build/untested"),
            (
                'python_executable="/opt/python/${PYTHON_TAG}-${PYTHON_TAG}/bin/python"',
                "python_executable=python3",
            ),
        ):
            with self.subTest(after=after):
                document = self.workflow()
                step = self.run_step(
                    document, "wheels", "buildsys/run_python_release.py"
                )
                self.assertIn(before, step["run"])
                step["run"] = step["run"].replace(before, after)
                with self.assertRaises(
                    check_python_release_workflow.WorkflowContractError
                ):
                    self.check_document(document)

    def test_duplicate_yaml_keys_are_rejected_before_contract_checks(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "duplicate.yml"
            path.write_text(
                "permissions: {contents: read}\npermissions: {contents: write}\n"
            )
            with self.assertRaisesRegex(
                check_python_release_workflow.WorkflowContractError,
                "duplicate workflow key: permissions",
            ):
                read_workflow(path)

    def test_protected_environment_and_tested_artifact_reference_are_required(
        self,
    ) -> None:
        document = self.workflow()
        document["jobs"]["publish-github-release"]["environment"] = "unprotected"
        with self.assertRaisesRegex(
            check_python_release_workflow.WorkflowContractError,
            "protected github-release",
        ):
            self.check_document(document)
        document = self.workflow()
        artifact = next(
            step
            for step in document["jobs"]["stage-github-release"]["steps"]
            if "uses" in step
        )
        artifact["with"]["name"] = "untested-wheel"
        with self.assertRaisesRegex(
            check_python_release_workflow.WorkflowContractError,
            "tested release bundle download",
        ):
            self.check_document(document)

    def test_manylinux_make_source_must_be_checksum_verified(self) -> None:
        content = (ROOT / ".github/workflows/python-wheels.yml").read_text(
            encoding="utf-8"
        )
        content = content.replace("sha256sum --check", "sha256sum --version")
        with tempfile.TemporaryDirectory() as temporary:
            workflow = Path(temporary) / "python-wheels.yml"
            workflow.write_text(content, encoding="utf-8")
            with self.assertRaisesRegex(
                check_python_release_workflow.WorkflowContractError,
                "GNU Make checksum verification",
            ):
                check_python_release_workflow.check(
                    workflow,
                    ROOT / "manifests/api/python-api-v1.json",
                    ROOT / ".github/workflows/pypi.yml",
                    ROOT / "buildsys/toolchains/llvm22-linux-64.lock",
                )

    def test_release_flags_must_keep_the_generic_x86_64_baseline(self) -> None:
        content = (ROOT / ".github/workflows/python-wheels.yml").read_text(
            encoding="utf-8"
        )
        content = content.replace("-march=x86-64", "-march=x86-64-v3")
        with tempfile.TemporaryDirectory() as temporary:
            workflow = Path(temporary) / "python-wheels.yml"
            workflow.write_text(content, encoding="utf-8")
            with self.assertRaisesRegex(
                check_python_release_workflow.WorkflowContractError,
                "generic x86-64 flags",
            ):
                check_python_release_workflow.check(
                    workflow,
                    ROOT / "manifests/api/python-api-v1.json",
                    ROOT / ".github/workflows/pypi.yml",
                    ROOT / "buildsys/toolchains/llvm22-linux-64.lock",
                )

    def test_release_epoch_must_fit_zip_timestamps(self) -> None:
        content = (ROOT / ".github/workflows/python-wheels.yml").read_text(
            encoding="utf-8"
        )
        content = content.replace(
            'SOURCE_DATE_EPOCH: "315532800"', 'SOURCE_DATE_EPOCH: "0"'
        )
        with tempfile.TemporaryDirectory() as temporary:
            workflow = Path(temporary) / "python-wheels.yml"
            workflow.write_text(content, encoding="utf-8")
            with self.assertRaisesRegex(
                check_python_release_workflow.WorkflowContractError,
                "ZIP-compatible source date epoch",
            ):
                check_python_release_workflow.check(
                    workflow,
                    ROOT / "manifests/api/python-api-v1.json",
                    ROOT / ".github/workflows/pypi.yml",
                    ROOT / "buildsys/toolchains/llvm22-linux-64.lock",
                )

    def test_node_actions_must_run_outside_manylinux(self) -> None:
        content = (ROOT / ".github/workflows/python-wheels.yml").read_text(
            encoding="utf-8"
        )
        content = content.replace(
            "  kernel:\n    needs: version\n",
            "  kernel:\n    container:\n      image: old-glibc\n    needs: version\n",
            1,
        )
        with tempfile.TemporaryDirectory() as temporary:
            workflow = Path(temporary) / "python-wheels.yml"
            workflow.write_text(content, encoding="utf-8")
            with self.assertRaisesRegex(
                check_python_release_workflow.WorkflowContractError,
                "Node-based Actions must run on the host",
            ):
                check_python_release_workflow.check(
                    workflow,
                    ROOT / "manifests/api/python-api-v1.json",
                    ROOT / ".github/workflows/pypi.yml",
                    ROOT / "buildsys/toolchains/llvm22-linux-64.lock",
                )

    def test_kernel_archive_must_support_manylinux_tar(self) -> None:
        content = (ROOT / ".github/workflows/python-wheels.yml").read_text(
            encoding="utf-8"
        )
        content = content.replace("tar --null --no-recursion", "tar --sort=name")
        with tempfile.TemporaryDirectory() as temporary:
            workflow = Path(temporary) / "python-wheels.yml"
            workflow.write_text(content, encoding="utf-8")
            with self.assertRaisesRegex(
                check_python_release_workflow.WorkflowContractError,
                "deterministic packaging supported by manylinux tar",
            ):
                check_python_release_workflow.check(
                    workflow,
                    ROOT / "manifests/api/python-api-v1.json",
                    ROOT / ".github/workflows/pypi.yml",
                    ROOT / "buildsys/toolchains/llvm22-linux-64.lock",
                )

    def test_llvm_toolchain_packages_must_be_checksum_pinned(self) -> None:
        content = (ROOT / "buildsys/toolchains/llvm22-linux-64.lock").read_text(
            encoding="utf-8"
        )
        content = content.replace("#1dd3fffd", "#unpinned", 1)
        with tempfile.TemporaryDirectory() as temporary:
            lock = Path(temporary) / "llvm.lock"
            lock.write_text(content, encoding="utf-8")
            with self.assertRaisesRegex(
                check_python_release_workflow.WorkflowContractError,
                "unpinned package",
            ):
                check_python_release_workflow.validate_toolchain_lock(lock)


if __name__ == "__main__":
    unittest.main()
