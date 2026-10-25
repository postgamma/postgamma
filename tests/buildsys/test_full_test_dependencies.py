"""Exercise package preservation, compiler separation and artifact loader checks."""

from __future__ import annotations

import json
import importlib.util
import os
import shlex
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "buildsys"))

from check_runtime_libraries import (
    RuntimeLibraryError, clean_environment, inspect_libraries, verify_loaded_libraries,
)

SCRIPT = ROOT / "scripts/full-test.sh"


def function(name: str, following: str) -> str:
    text = SCRIPT.read_text()
    return text[text.index(name + "()\n"):text.index(following + "()\n")]


class FullTestDependencyTests(unittest.TestCase):
    @unittest.skipUnless(importlib.util.find_spec("pip"), "requires pip")
    def test_runtime_python_probe_checks_abi_and_loaded_toolchain_libraries(self):
        abi = f"{sys.version_info.major}.{sys.version_info.minor}"
        with tempfile.TemporaryDirectory() as temporary:
            for expected_abi, forbidden, succeeds in (
                (abi, temporary, True),
                ("0.0", temporary, False),
                (abi, str(Path(sys.executable).resolve().parent.parent), False),
            ):
                with self.subTest(abi=expected_abi, forbidden=forbidden):
                    result = subprocess.run(
                        ["bash", "-c", "set -Eeuo pipefail\n"
                         + 'toolchain=$1\n' + function("runtime_python_works", "install_runtime_python")
                         + '\nruntime_python_works "$2" "$3"',
                         "runtime-probe", forbidden, sys.executable, expected_abi],
                        text=True, capture_output=True,
                    )
                    self.assertEqual(result.returncode == 0, succeeds, result.stderr)

    def test_runtime_python_reuses_verified_interpreters_without_provisioning(self):
        installer = function("install_runtime_python", "install_validation_dependencies")
        candidates = '"/usr/bin/python$abi" /usr/bin/python3'
        self.assertEqual(installer.count(candidates), 1)
        # Redirect only discovery paths; retain the real executable-file checks.
        installer = installer.replace(candidates, '"$system_bin/python$abi" "$system_bin/python3"')
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            toolchain = root / "toolchain"
            (toolchain / "bin").mkdir(parents=True)
            (toolchain / "bin/python3").write_text("#!/bin/sh\nprintf '3.11\\n'\n")
            (toolchain / "bin/python3").chmod(0o755)
            system_bin = root / "system-bin"
            system_bin.mkdir()
            versioned = system_bin / "python3.11"
            generic = system_bin / "python3"
            cached = root / "runtime-python"
            (cached / "bin").mkdir(parents=True)
            cached_python = cached / "bin/python3.11"
            cached_python.write_text("#!/bin/sh\nexit 0\n")
            cached_python.chmod(0o755)
            (cached / ".postgamma-runtime-python").write_text("pinned-sha\n")
            for selection in (
                "explicit", "system-versioned", "system", "cached",
                "cached-missing-system", "cached-nonexecutable-system", "invalid-explicit",
            ):
                with self.subTest(selection=selection):
                    for candidate in (versioned, generic):
                        candidate.unlink(missing_ok=True)
                        if selection != "cached-missing-system":
                            candidate.write_text("#!/bin/sh\nexit 0\n")
                            candidate.chmod(0o644 if selection == "cached-nonexecutable-system" else 0o755)
                    selected = sys.executable
                    expected_probes = [sys.executable]
                    if selection == "system-versioned":
                        selected = str(versioned)
                        expected_probes = [str(versioned)]
                    elif selection == "system":
                        selected = str(generic)
                        expected_probes = [str(versioned), str(generic)]
                    elif selection.startswith("cached"):
                        selected = str(cached_python)
                        expected_probes = [str(cached_python)]
                        if selection == "cached":
                            expected_probes[:0] = [str(versioned), str(generic)]
                    probes = root / "probes"
                    probes.unlink(missing_ok=True)
                    environment = {**os.environ, "SELECTED": selected}
                    environment.pop("POSTGAMMA_RUNTIME_PYTHON", None)
                    if selection in ("explicit", "invalid-explicit"):
                        environment["POSTGAMMA_RUNTIME_PYTHON"] = sys.executable
                    if selection == "invalid-explicit":
                        environment["SELECTED"] = "/does-not-exist"
                    result = subprocess.run(
                        ["bash", "-c", "set -Eeuo pipefail\n"
                         + 'state_root=$1; toolchain=$1/toolchain; tools_root=$1/tools;\n'
                         + 'system_bin=$1/system-bin;\n'
                         + 'RUNTIME_PYTHON_VERSION=3.11.16; RUNTIME_PYTHON_SHA256=pinned-sha;\n'
                         + 'log() { :; }; fail() { printf "%s\\n" "$*" >&2; exit 2; };\n'
                         + 'runtime_python_works() { printf "%s\\n" "$1" >> "$state_root/probes"; '
                         + '[[ "$1" == "$SELECTED" && "$2" == 3.11 ]]; };\n'
                         + 'install_packages() { exit 97; }; download_file() { exit 98; };\n'
                         + installer
                         + '\ninstall_runtime_python\nprintf "%s\\n" "$runtime_python"',
                         "runtime-selection", str(root)],
                        env=environment, text=True, capture_output=True,
                    )
                    if selection == "invalid-explicit":
                        self.assertEqual(result.returncode, 2, result.stderr)
                        self.assertIn("needs ABI 3.11", result.stderr)
                    else:
                        self.assertEqual(result.returncode, 0, result.stderr)
                        self.assertEqual(result.stdout.strip(), selected)
                    self.assertEqual(probes.read_text().splitlines(), expected_probes)
            self.assertFalse((root / "tools").exists())

    def run_package_install(
        self, family: str, *, changed: bool = False, all_present: bool = False,
        documentation: bool = False, os_id: str = "centos", os_version: str = "7",
        doxygen_present: bool = False,
    ):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            source = SCRIPT.read_text()
            documentation_setup = source[
                source.index("documentation_repository=\n"):source.index("((EUID != 0))")
            ]
            harness = (
                "set -Eeuo pipefail\n"
                "work_root=$1; package_family=$2; package_manager=dnf\n"
                "os_id=$3; os_version=$4; mode=$5\n"
                "log() { :; }; fail() { printf '%s\\n' \"$*\" >&2; exit 2; }\n"
                "packages=(git strace)\n"
                + documentation_setup +
                "snapshot_packages() { cat \"$work_root/snapshot\"; }\n"
                "run_privileged() { printf '%s\\n' \"$*\" >> \"$work_root/commands\"; "
                "if [[ ${NEED_CRB:-0} == 1 && \" $* \" == *' doxygen '* && "
                "\" $* \" != *' --enablerepo=crb '* ]]; then return 1; fi; "
                "if [[ ${CHANGE_PACKAGES:-0} == 1 ]]; then "
                "printf 'git\\t2.43\\nopenssl-libs\\t3.5\\n' > \"$work_root/snapshot\"; fi; }\n"
                + function("install_packages", "download_file")
                + "\ninstall_packages\n"
            )
            snapshot = "git\t2.43\nopenssl-libs\t3.0\n"
            if all_present:
                snapshot += "strace\t4.24\n"
            if doxygen_present or (all_present and documentation):
                snapshot = "doxygen\t1.9.1\n" + snapshot
            (directory / "snapshot").write_text(snapshot)
            result = subprocess.run(
                ["bash", "-c", harness, "packages", temporary, family, os_id,
                 os_version, "run" if documentation else "artifacts"],
                env={**os.environ, "CHANGE_PACKAGES": str(int(changed)),
                     "POSTGAMMA_SKIP_PACKAGES": "0",
                     "NEED_CRB": str(int(os_id == "rocky" and os_version.split(".")[0] == "9"))},
                text=True, capture_output=True,
            )
            commands = directory / "commands"
            preferences = list(directory.glob("run/packages/*/preferences"))
            return (result, commands.read_text() if commands.exists() else "",
                    preferences[0].read_text() if preferences else "")

    def test_apt_installs_only_missing_packages_and_pins_transitive_dependencies(self):
        result, commands, pins = self.run_package_install("apt")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("--no-remove --no-install-recommends install -y strace", commands)
        self.assertIn("Package: openssl-libs\nPin: version 3.0\nPin-Priority: 1001", pins)
        self.assertIn("Package: openssl-libs\nPin: version *\nPin-Priority: -1", pins)
        self.assertNotIn("install -y git", commands)

    def test_rpm_excludes_and_protects_installed_dependency_packages(self):
        result, commands, _ = self.run_package_install("rpm")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("--exclude=git,openssl-libs", commands)
        self.assertIn("--setopt=protected_packages=git,openssl-libs", commands)
        self.assertIn("--setopt=obsoletes=0 install -y strace", commands)

    def test_existing_packages_do_not_invoke_a_privileged_command(self):
        result, commands, _ = self.run_package_install("apt", all_present=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(commands, "")

    def test_rocky9_missing_doxygen_uses_crb_with_package_preservation(self):
        for changed in (False, True):
            with self.subTest(changed_installed_package=changed):
                result, commands, _ = self.run_package_install(
                    "rpm", documentation=True, os_id="rocky", os_version="9.8",
                    changed=changed,
                )
                self.assertEqual(result.returncode, 2 if changed else 0, result.stderr)
                self.assertEqual(len(commands.splitlines()), 1)
                self.assertIn("dnf --enablerepo=crb --exclude=git,openssl-libs", commands)
                self.assertIn("--setopt=protected_packages=git,openssl-libs", commands)
                self.assertIn("--setopt=obsoletes=0 install -y strace doxygen", commands)
                if changed:
                    self.assertIn("installed packages changed", result.stderr)

    def test_documentation_repository_is_not_enabled_for_other_installations(self):
        for os_id, version, documentation, all_present in (
            ("rocky", "9.8", False, False),
            ("rocky", "9.8", True, True),
            ("rocky", "8.10", True, False),
            ("centos", "7.2", True, False),
            ("fedora", "42", True, False),
            ("rhel", "9.8", True, False),
        ):
            with self.subTest(os_id=os_id, version=version,
                              documentation=documentation, all_present=all_present):
                result, commands, _ = self.run_package_install(
                    "rpm", documentation=documentation, os_id=os_id,
                    os_version=version, all_present=all_present,
                )
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertNotIn("--enablerepo", commands)
                if all_present:
                    self.assertEqual(commands, "")
        result, commands, _ = self.run_package_install(
            "rpm", documentation=True, os_id="rocky", os_version="9.8",
            doxygen_present=True,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("install -y strace", commands)
        self.assertNotIn("--enablerepo", commands)

    def test_changed_installed_package_cannot_produce_success(self):
        result, _, _ = self.run_package_install("rpm", changed=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("installed packages changed", result.stderr)

    @unittest.skipUnless(shutil.which("apt-get"), "requires the APT solver")
    def test_real_apt_solver_cannot_upgrade_an_indirect_dependency(self):
        _, _, pins = self.run_package_install("apt")
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            repo = directory / "repo"
            repo.mkdir()
            (directory / "state/lists/partial").mkdir(parents=True)
            (directory / "cache/archives/partial").mkdir(parents=True)
            packages = []
            for name, version, dependency in (
                ("openssl-libs", "3.5", ""),
                ("strace", "1", "openssl-libs (>= 3.5)"),
                ("postgamma-test-headers", "1", "openssl-libs (>= 3.0)"),
            ):
                packages.append(
                    f"Package: {name}\nVersion: {version}\nArchitecture: amd64\n"
                    f"Filename: {name}.deb\nSize: 1\nDescription: dependency fixture\n"
                    + (f"Depends: {dependency}\n" if dependency else "")
                )
            (repo / "Packages").write_text("\n".join(packages))
            (directory / "sources.list").write_text(f"deb [trusted=yes] file:{repo} ./\n")
            status = directory / "status"
            status.write_text(
                "Package: openssl-libs\nStatus: install ok installed\n"
                "Version: 3.0\nArchitecture: amd64\nDescription: installed library\n\n"
            )
            preferences = directory / "preferences"
            preferences.write_text("")
            options = [
                "apt-get", "-o", f"Dir::State={directory / 'state'}",
                "-o", f"Dir::State::status={status}",
                "-o", f"Dir::Cache={directory / 'cache'}",
                "-o", "Dir::Etc::main=/dev/null", "-o", "Dir::Etc::parts=-",
                "-o", f"Dir::Etc::sourcelist={directory / 'sources.list'}",
                "-o", "Dir::Etc::sourceparts=-",
                "-o", f"Dir::Etc::preferences={preferences}",
                "-o", "Dir::Etc::preferencesparts=-",
                "-o", "APT::Architecture=amd64",
                "-o", f"APT::Sandbox::User={os.environ.get('USER', 'nobody')}",
            ]
            update = subprocess.run([*options, "update"], text=True, capture_output=True, timeout=60)
            self.assertEqual(update.returncode, 0, update.stdout + update.stderr)
            unsafe = subprocess.run([*options, "--simulate", "install", "strace"],
                                    text=True, capture_output=True, timeout=30)
            self.assertEqual(unsafe.returncode, 0, unsafe.stderr)
            self.assertIn("Inst openssl-libs", unsafe.stdout)
            preferences.write_text(pins)
            rejected = subprocess.run([*options, "--simulate", "--no-remove", "install", "strace"],
                                      text=True, capture_output=True, timeout=30)
            self.assertNotEqual(rejected.returncode, 0)
            accepted = subprocess.run(
                [*options, "--simulate", "--no-remove", "install", "postgamma-test-headers"],
                text=True, capture_output=True, timeout=30,
            )
            self.assertEqual(accepted.returncode, 0, accepted.stderr)
            self.assertNotIn("Inst openssl-libs", accepted.stdout)
            self.assertIn("Version: 3.0", status.read_text())

    def test_compiler_wrappers_keep_product_and_ast_dependencies_separate(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            toolchain = directory / "toolchain"
            (toolchain / "bin").mkdir(parents=True)
            recorder = '#!/bin/sh\nprintf "%s\\n" "$@"\n'
            for name in ("clang", "clang++"):
                (toolchain / "bin" / name).write_text(recorder)
                (toolchain / "bin" / name).chmod(0o755)
            subprocess.run(
                ["bash", "-c", "set -Eeuo pipefail\n"
                 + "toolchain=$1; system_bridge=$2; SCRIPT_NAME=full-test.sh; fail() { exit 2; };\n"
                 + function("install_compiler_wrappers", "raise_open_file_limit")
                 + "\ninstall_compiler_wrappers", "wrappers", str(toolchain), str(directory / "system")],
                check=True, capture_output=True, text=True,
            )
            arguments = subprocess.check_output(
                [str(directory / "system/bin/postgamma-cc"), "fixture.c", "-o", "fixture"], text=True,
            ).splitlines()
            self.assertIn("--sysroot=/", arguments)
            self.assertIn("--gcc-toolchain=/usr", arguments)
            self.assertFalse(any(str(toolchain / "lib") in value for value in arguments))
            self.assertNotIn(str(toolchain / "include"), arguments)
            arguments = subprocess.check_output(
                [str(toolchain / "bin/postgamma-ast-clangxx"), "fixture.o", "-o", "tool"], text=True,
            ).splitlines()
            self.assertIn(f"-Wl,-rpath,{toolchain / 'lib'}", arguments)

    def test_sufficient_system_git_does_not_build_or_install_a_replacement(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            binary = directory / "git"
            binary.write_text("#!/bin/sh\nprintf 'git version 2.28.0\\n'\n")
            binary.chmod(0o755)
            result = subprocess.run(
                ["bash", "-c", "set -Eeuo pipefail\n"
                 + "tools_root=$1/tools; git_prefix=$1/fallback; GIT_VERSION=2.43.7; log() { :; };\n"
                 + function("install_git", "install_perl_modules")
                 + '\ninstall_git\nprintf "%s\\n" "$git_executable"', "git-selection", temporary],
                env={**os.environ, "PATH": f"{temporary}:/usr/bin:/bin"},
                text=True, capture_output=True,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stdout.strip(), str(binary))
            self.assertFalse((directory / "fallback").exists())

    def test_runtime_audit_rejects_build_library_even_with_a_baked_in_search_path(self):
        compiler = shlex.split(os.environ.get("CC", "cc"))
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            source = directory / "library.c"
            source.write_text("int fixture_value(void) { return 7; }\n")
            subprocess.run([*compiler, "-shared", "-fPIC", str(source),
                            "-Wl,-soname,libfixture.so", "-o", str(directory / "libfixture.so")], check=True)
            source.write_text("extern int fixture_value(void); int main(void) { return fixture_value() != 7; }\n")
            binary = directory / "consumer"
            subprocess.run([*compiler, str(source), f"-L{directory}", "-lfixture",
                            f"-Wl,-rpath,{directory}", "-o", str(binary)], check=True)
            with self.assertRaisesRegex(RuntimeLibraryError, "inside build tools"):
                inspect_libraries(binary, forbidden=[directory])
            report = inspect_libraries(binary)
            self.assertEqual(report["resolved"]["libfixture.so"], str(directory / "libfixture.so"))
            with self.assertRaisesRegex(RuntimeLibraryError, "exceeds the requested baseline"):
                inspect_libraries(binary, maximum_glibc=(0,))

    def test_runtime_audit_does_not_allow_ld_library_path_to_hide_missing_dependency(self):
        compiler = shlex.split(os.environ.get("CC", "cc"))
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            source = directory / "library.c"
            source.write_text("int fixture_value(void) { return 1; }\n")
            subprocess.run([*compiler, "-shared", "-fPIC", str(source),
                            "-Wl,-soname,libfixture.so", "-o", str(directory / "libfixture.so")], check=True)
            source.write_text("extern int fixture_value(void); int main(void) { return fixture_value(); }\n")
            binary = directory / "consumer"
            subprocess.run([*compiler, str(source), f"-L{directory}", "-lfixture", "-o", str(binary)], check=True)
            with mock.patch.dict(os.environ, {"LD_LIBRARY_PATH": temporary}):
                with self.assertRaisesRegex(RuntimeLibraryError, "cannot resolve"):
                    inspect_libraries(binary)
                self.assertNotIn("LD_LIBRARY_PATH", clean_environment())

    def test_process_audit_detects_an_already_loaded_library_that_ldd_cannot_see(self):
        compiler = shlex.split(os.environ.get("CC", "cc"))
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            source = directory / "fixture.c"
            for name, value in (("system", 1), ("private", 2)):
                target = directory / name
                target.mkdir()
                source.write_text(f"int fixture_value(void) {{ return {value}; }}\n")
                subprocess.run([
                    *compiler, "-shared", "-fPIC", str(source),
                    "-Wl,-soname,libfixture.so", "-o", str(target / "libfixture.so"),
                ], check=True)
            source.write_text("extern int fixture_value(void); int result(void) { return fixture_value(); }\n")
            consumer = directory / "consumer.so"
            subprocess.run([
                *compiler, "-shared", "-fPIC", str(source), f"-L{directory / 'system'}",
                "-lfixture", f"-Wl,-rpath,{directory / 'system'}", "-o", str(consumer),
            ], check=True)
            expected = inspect_libraries(consumer)["resolved"]
            probe = (
                "import ctypes, json, sys\n"
                "from pathlib import Path\n"
                "preloaded = ctypes.CDLL(sys.argv[1])\n"
                "consumer = ctypes.CDLL(sys.argv[2])\n"
                "objects = sorted({fields[5] for line in Path('/proc/self/maps').read_text().splitlines() "
                "if len(fields := line.split(maxsplit=5)) == 6 "
                "and 'x' in fields[1] and fields[5].startswith('/')})\n"
                "print(json.dumps({'value': consumer.result(), 'objects': objects}))\n"
            )
            for name, value in (("system", 1), ("private", 2)):
                result = json.loads(subprocess.check_output([
                    sys.executable, "-c", probe, str(directory / name / "libfixture.so"), str(consumer),
                ], env=clean_environment(), text=True))
                self.assertEqual(result["value"], value)
                if name == "private":
                    with self.assertRaisesRegex(RuntimeLibraryError, "process loaded"):
                        verify_loaded_libraries(result["objects"], expected)
                else:
                    self.assertEqual(verify_loaded_libraries(result["objects"], expected), expected)
                    with self.assertRaisesRegex(RuntimeLibraryError, "did not report"):
                        verify_loaded_libraries([], expected)


if __name__ == "__main__":
    unittest.main()
