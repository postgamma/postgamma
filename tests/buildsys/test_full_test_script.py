"""Tests for the one-command complete source-test provisioner."""

from __future__ import annotations

import os
import re
import subprocess
import tempfile
import unittest
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parents[2]
SCRIPT = PROJECT_ROOT / "scripts" / "full-test.sh"
TOOLCHAIN_LOCK = (
    PROJECT_ROOT / "buildsys" / "toolchains" / "full-test-linux-64.lock"
)


class FullTestScriptTests(unittest.TestCase):
    def test_workflow_dependencies_use_the_managed_interpreter_without_sudo(self) -> None:
        source = SCRIPT.read_text()
        function = source[source.index("install_workflow_dependencies()\n"):source.index("install_git()\n")]
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            binary = root / "bin/python3"
            binary.parent.mkdir()
            binary.write_text('#!/bin/sh\nprintf "%s\\n" "$@"\n')
            binary.chmod(0o755)
            result = subprocess.run(
                ["bash", "-c", 'toolchain=$1; source_dir=$2; log() { :; };\n' + function + '\ninstall_workflow_dependencies',
                 "workflow-install", str(root), str(PROJECT_ROOT)],
                check=True, text=True, capture_output=True,
            )
            self.assertEqual(result.stdout.splitlines(), [
                "-m", "pip", "install", "--disable-pip-version-check", "--requirement",
                str(PROJECT_ROOT / "buildsys/requirements.txt"),
            ])

    def run_plan(
        self,
        os_release: str,
        *,
        package_manager: str | None = None,
        work_root: Path | None = None,
        state_root: Path | None = None,
    ) -> subprocess.CompletedProcess[str]:
        with tempfile.TemporaryDirectory() as temporary:
            release = Path(temporary) / "os-release"
            release.write_text(os_release, encoding="utf-8")
            environment = {
                **os.environ,
                "POSTGAMMA_OS_RELEASE": str(release),
                "POSTGAMMA_JOBS": "2",
            }
            environment.pop("POSTGAMMA_STATE_ROOT", None)
            environment.pop("POSTGAMMA_WORK_ROOT", None)
            if work_root is not None:
                environment["POSTGAMMA_WORK_ROOT"] = str(work_root)
            if state_root is not None:
                environment["POSTGAMMA_STATE_ROOT"] = str(state_root)
            for variable in (
                "PG_REPOSITORY",
                "PG_REPOSITORY_FALLBACK",
                "PGVECTOR_REPOSITORY",
                "PGVECTOR_REPOSITORY_FALLBACK",
            ):
                environment.pop(variable, None)
            if package_manager:
                environment["POSTGAMMA_PACKAGE_MANAGER"] = package_manager
            return subprocess.run(
                [str(SCRIPT), "--plan"],
                cwd=PROJECT_ROOT,
                env=environment,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                check=False,
            )

    def test_ubuntu_plan_uses_apt_packages(self) -> None:
        result = self.run_plan(
            'ID=ubuntu\nID_LIKE=debian\nVERSION_ID="24.04"\nNAME="Ubuntu"\n'
        )
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn("package_family=apt\n", result.stdout)
        self.assertIn("package_manager=apt-get\n", result.stdout)
        self.assertIn("build-essential", result.stdout)
        self.assertIn("libcurl4-openssl-dev", result.stdout)
        self.assertIn(" perl ", result.stdout)
        self.assertIn(f"work_root={PROJECT_ROOT / 'build/full-test'}\n", result.stdout)
        state_root = PROJECT_ROOT / "build/full-test/state"
        self.assertIn(f"state_root={state_root}\n", result.stdout)
        self.assertIn(f"git_prefix={state_root / 'git'}\n", result.stdout)
        self.assertIn(f"perl_modules={state_root / 'perl'}\n", result.stdout)
        self.assertIn("jobs=2\n", result.stdout)
        self.assertIn("swap_swappiness=60\n", result.stdout)
        self.assertIn("session_executor_timeout_seconds=3600\n", result.stdout)
        self.assertIn(
            "session_executor_request_timeout_seconds=300\n", result.stdout
        )
        self.assertIn("logical_management_timeout_seconds=1800\n", result.stdout)
        self.assertIn("postgres_source=submodule-default\n", result.stdout)
        self.assertIn("postgres_source_fallback=none\n", result.stdout)
        self.assertIn("pgvector_source=submodule-default\n", result.stdout)
        self.assertIn("pgvector_source_fallback=none\n", result.stdout)

    def test_custom_work_root_contains_default_toolchain_state(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            work_root = Path(temporary) / "full-test"
            result = self.run_plan(
                'ID=ubuntu\nID_LIKE=debian\nVERSION_ID="24.04"\nNAME="Ubuntu"\n',
                work_root=work_root,
            )

            self.assertEqual(result.returncode, 0, result.stdout)
            self.assertIn(f"work_root={work_root}\n", result.stdout)
            self.assertIn(f"state_root={work_root / 'state'}\n", result.stdout)

    def test_explicit_state_root_is_preserved(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            state_root = Path(temporary) / "shared-toolchain"
            result = self.run_plan(
                'ID=ubuntu\nID_LIKE=debian\nVERSION_ID="24.04"\nNAME="Ubuntu"\n',
                state_root=state_root,
            )

            self.assertEqual(result.returncode, 0, result.stdout)
            self.assertIn(f"state_root={state_root}\n", result.stdout)

    def test_anolis_7_plan_uses_legacy_rpm_names(self) -> None:
        result = self.run_plan(
            'ID=anolis\nID_LIKE="rhel fedora centos"\n'
            'VERSION_ID="7.7"\nNAME="Anolis OS"\n',
            package_manager="yum",
        )
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn("package_family=rpm\n", result.stdout)
        self.assertIn("package_manager=yum\n", result.stdout)
        packages = next(
            line for line in result.stdout.splitlines() if line.startswith("packages=")
        ).split()
        self.assertIn("pkgconfig", packages)
        self.assertNotIn("pkgconf-pkg-config", packages)

    def test_amazon_2023_plan_uses_modern_rpm_names(self) -> None:
        result = self.run_plan(
            'ID=amzn\nID_LIKE="fedora"\nVERSION_ID="2023"\n'
            'NAME="Amazon Linux"\n',
            package_manager="dnf",
        )
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn("package_manager=dnf\n", result.stdout)
        self.assertIn("pkgconf-pkg-config", result.stdout)

    def test_opensuse_plan_uses_zypper(self) -> None:
        result = self.run_plan(
            'ID=opensuse-leap\nID_LIKE="suse opensuse"\n'
            'VERSION_ID="16.0"\nNAME="openSUSE Leap"\n'
        )
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn("package_family=zypper\n", result.stdout)
        self.assertIn("package_manager=zypper\n", result.stdout)

    def test_unknown_distribution_stops_before_installation(self) -> None:
        result = self.run_plan(
            'ID=unknown\nID_LIKE="independent"\nVERSION_ID="1"\n'
        )
        self.assertEqual(result.returncode, 2)
        self.assertIn("unsupported Linux distribution", result.stdout)

    def test_toolchain_lock_is_explicit_and_complete(self) -> None:
        lines = [
            line.strip()
            for line in TOOLCHAIN_LOCK.read_text(encoding="utf-8").splitlines()
            if line.strip() and not line.startswith("#")
        ]
        self.assertEqual(lines[0], "@EXPLICIT")
        entries = lines[1:]
        self.assertEqual(len(entries), len(set(entries)))
        pattern = re.compile(
            r"^https://conda\.anaconda\.org/conda-forge/"
            r"(?:linux-64|noarch)/[^#]+#[0-9a-f]{64}$"
        )
        self.assertTrue(all(pattern.fullmatch(entry) for entry in entries))
        for marker in (
            "/python-3.11.16-",
            "/make-4.4.1-",
            "/perl-5.32.1-",
            "/clang-22.1.8-",
            "/clangxx-22.1.8-",
            "/clangdev-22.1.8-",
            "/llvmdev-22.1.8-",
            "/sysroot_linux-64-2.17-",
            "/readline-8.3-",
        ):
            self.assertTrue(
                any(marker in entry for entry in entries),
                f"toolchain lock is missing {marker}",
            )

    def test_non_system_perl_module_is_checksum_pinned(self) -> None:
        content = SCRIPT.read_text(encoding="utf-8")
        self.assertIn("IPC_RUN_VERSION=0.94", content)
        self.assertIn(
            "IPC_RUN_SHA256="
            "2eb336c91a2b7ea61f98e5b2282d91020d39a484f16041e2365ffd30f8a5605b",
            content,
        )
        self.assertIn(
            "https://cpan.metacpan.org/authors/id/T/TO/TODDR/IPC-Run-",
            content,
        )

    def test_source_built_git_is_checksum_pinned(self) -> None:
        content = SCRIPT.read_text(encoding="utf-8")
        self.assertIn("GIT_VERSION=2.43.7", content)
        self.assertIn('CFLAGS="-g -O2 -Wall -std=gnu99"', content)
        self.assertIn(
            "GIT_SHA256="
            "657e2374455d9e62f6cdb3e7c55d867b6db5404d744e97e112cc5b0db687a19f",
            content,
        )
        self.assertIn(
            "https://www.kernel.org/pub/software/scm/git/git-",
            content,
        )
        self.assertNotIn("run_as_test_user", content)
        self.assertNotIn("setpriv", content)
        self.assertNotIn("useradd", content)
        self.assertIn('export USER=$(printf \'%q\' "$test_user")', content)

    def test_bootstrap_downloads_retry_transport_failures(self) -> None:
        content = SCRIPT.read_text(encoding="utf-8")
        download = content.split("download_file()", maxsplit=1)[1].split(
            "prepare_swap()", maxsplit=1
        )[0]

        self.assertIn("local -a curl_options=(--fail --location)", download)
        harness = (
            "set -Eeuo pipefail\n"
            "log() { :; }\n"
            "fail() { exit 2; }\n"
            "sleep() { :; }\n"
            "download_file()" + download + "\n"
            "attempts=0\n"
            "curl() {\n"
            "  if [[ ${1:-} == --http1.1 ]]; then return 2; fi\n"
            "  [[ ${1:-} == --fail && ${2:-} == --location ]] || return 3\n"
            "  attempts=$((attempts + 1))\n"
            "  ((attempts == 2))\n"
            "}\n"
            "download_file /tmp/unused https://example.invalid/test test\n"
            "[[ $attempts == 2 ]]\n"
        )
        result = subprocess.run(
            ["bash", "-c", harness],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            check=False,
        )

        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertEqual(content.count("\tdownload_file "), 3)
        self.assertNotIn("curl --fail --location --retry 3", content)

    def test_low_memory_swap_policy_is_temporary(self) -> None:
        content = SCRIPT.read_text(encoding="utf-8")

        self.assertIn("prepare_swap_policy", content)
        self.assertIn('trap cleanup EXIT', content)
        self.assertIn('vm.swappiness=$swap_swappiness', content)
        self.assertIn("memory.swappiness", content)

    def test_sudo_session_survives_long_provisioning_without_late_prompts(self) -> None:
        content = SCRIPT.read_text(encoding="utf-8")
        provision = content.split("ensure_sudo_session()", maxsplit=1)[1].split(
            "install_packages()", maxsplit=1
        )[0]
        cleanup = content.split("restore_swap_policy()", maxsplit=1)[1].split(
            "prepare_swap_policy()", maxsplit=1
        )[0]
        harness = (
            "set -Eeuo pipefail\n"
            "log() { :; }\n"
            "fail() { exit 2; }\n"
            "sleep() { return 1; }\n"
            "sudo() { printf '%s\\n' \"$*\" >> \"$test_log\"; }\n"
            "sudo_command=sudo\n"
            "sudo_keepalive_pid=\n"
            "sudo_owner_pid=$$\n"
            "original_cgroup_swappiness=\n"
            "original_global_swappiness=\n"
            "swappiness_cgroup_file=\n"
            "ensure_sudo_session()" + provision + "\n"
            "restore_swap_policy()" + cleanup + "\n"
            "run_privileged true\n"
            "run_privileged prlimit --pid 123 --nofile=1048576:1048576\n"
            "original_global_swappiness=0\n"
            "cleanup\n"
            "trap - EXIT\n"
        )
        with tempfile.TemporaryDirectory() as temporary:
            log_path = Path(temporary) / "sudo.log"
            result = subprocess.run(
                ["bash", "-c", harness],
                env={**os.environ, "test_log": str(log_path)},
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                check=False,
            )
            self.assertEqual(result.returncode, 0, result.stdout)
            self.assertEqual(
                log_path.read_text(encoding="utf-8").splitlines(),
                [
                    "-v",
                    "-n -- true",
                    "-n -- prlimit --pid 123 --nofile=1048576:1048576",
                    "-n -- sysctl -q -w vm.swappiness=0",
                ],
            )
        self.assertIn('"$sudo_command" -n -v', provision)
        self.assertIn('kill -0 "$sudo_owner_pid"', provision)

    def test_low_hard_nofile_limit_is_raised_for_only_the_test_process(self) -> None:
        content = SCRIPT.read_text(encoding="utf-8")
        raise_limit = content.split("raise_open_file_limit()", maxsplit=1)[1].split(
            "write_runner()", maxsplit=1
        )[0]

        self.assertIn('run_privileged prlimit --pid "$$"', raise_limit)
        self.assertIn('ulimit -Sn "$REQUIRED_NOFILE"', raise_limit)
        self.assertNotIn("limits.conf", content)

    def test_runner_clears_inherited_locale_categories(self) -> None:
        content = SCRIPT.read_text(encoding="utf-8")
        runner = content.split("write_runner()", maxsplit=1)[1]
        unset = runner.index("unset LANGUAGE LC_ADDRESS")
        export = runner.index("export LC_ALL=C")
        self.assertLess(unset, export)
        for category in (
            "LC_COLLATE",
            "LC_CTYPE",
            "LC_IDENTIFICATION",
            "LC_MEASUREMENT",
            "LC_MESSAGES",
            "LC_MONETARY",
            "LC_NAME",
            "LC_NUMERIC",
            "LC_PAPER",
            "LC_TELEPHONE",
            "LC_TIME",
        ):
            self.assertIn(category, runner[unset:export])

    def test_repository_ref_accepts_a_commit_or_remote_branch(self) -> None:
        content = SCRIPT.read_text(encoding="utf-8")
        self.assertIn('git rev-parse --verify "${repository_ref}^{commit}"', content)
        self.assertIn(
            'git rev-parse --verify "origin/${repository_ref}^{commit}"', content
        )
        self.assertNotIn('git clone --branch "$repository_ref"', content)

    def test_checkout_runs_in_place_without_a_second_clone(self) -> None:
        content = SCRIPT.read_text(encoding="utf-8")
        prepare_source = content.split("prepare_source()", maxsplit=1)[1].split(
            "install_micromamba()", maxsplit=1
        )[0]

        self.assertIn("source_dir=$checkout_root", content)
        self.assertNotIn('git clone --no-hardlinks "$checkout_root"', content)
        self.assertNotIn("git -C", prepare_source)
        self.assertIn('(cd "$source_dir" && git rev-parse', prepare_source)

    def test_local_repository_inputs_are_staged_before_source_checkout(self) -> None:
        content = SCRIPT.read_text(encoding="utf-8")
        self.assertIn("stage_local_repository()", content)
        self.assertIn('[[ -n "$value" ]] || return 0', content)
        self.assertIn('[[ -e "$value" ]] || return 0', content)
        self.assertIn('cp -a -- "$source" "$destination"', content)
        self.assertNotIn("chown -R", content)
        stage = content.index("stage_repository_inputs\n")
        prepare = content.index("prepare_source\n", stage)
        self.assertLess(stage, prepare)

    def test_runner_requires_the_calling_user_and_never_creates_an_account(self) -> None:
        content = SCRIPT.read_text(encoding="utf-8")
        self.assertIn("((EUID != 0))", content)
        self.assertIn("do not run as root", content)
        self.assertNotIn("POSTGAMMA_TEST_USER", content)
        self.assertNotIn("create_test_user", content)
        self.assertNotIn("useradd", content)
        self.assertIn("run_privileged", content)

    def test_repository_fallbacks_are_explicit_parameters(self) -> None:
        content = SCRIPT.read_text(encoding="utf-8")
        for variable in (
            "POSTGAMMA_REPOSITORY_FALLBACK",
            "PG_REPOSITORY_FALLBACK",
            "PGVECTOR_REPOSITORY_FALLBACK",
        ):
            self.assertIn(variable, content)

    def test_source_overrides_do_not_leak_into_the_test_gate(self) -> None:
        runner = SCRIPT.read_text(encoding="utf-8").split(
            "write_runner()", maxsplit=1
        )[1]
        source_init = runner.index('"\\$MAKE" source-init')
        clear = runner.index("unset PG_REPOSITORY PG_REPOSITORY_FALLBACK")
        complete = runner.index('exec "\\$MAKE" -j1 complete-source-check')
        self.assertLess(source_init, clear)
        self.assertLess(clear, complete)


if __name__ == "__main__":
    unittest.main()
