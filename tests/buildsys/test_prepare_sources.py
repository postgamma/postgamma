"""Exercise source setup and canary entry points with local Git repositories."""

from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parents[2]


class PrepareSourcesTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)
        self.root = self.directory / "project"
        self.environment = dict(os.environ)
        for name in (
            "MAKEFLAGS", "MFLAGS", "MAKEOVERRIDES", "PG_REPOSITORY",
            "PGVECTOR_REPOSITORY", "PG_BRANCH", "PG_CONFIGURE_ARGS",
            "CANARY_PROFILE", "CANARY_STRESS_SECONDS",
        ):
            self.environment.pop(name, None)
        count = int(self.environment.get("GIT_CONFIG_COUNT", "0"))
        self.environment.update({
            "GIT_CONFIG_COUNT": str(count + 1),
            f"GIT_CONFIG_KEY_{count}": "protocol.file.allow",
            f"GIT_CONFIG_VALUE_{count}": "always",
        })
        self.mirrors = {}
        self.pins = {}
        for name in ("postgres", "third_party/pgvector"):
            repository = self.directory / (Path(name).name + "-mirror")
            self.initialize(repository)
            (repository / "source.c").write_text("int pinned_source;\n")
            self.commit(repository)
            self.mirrors[name] = repository
            self.pins[name] = self.git(repository, "rev-parse", "HEAD")
        self.initialize(self.root)
        modules = []
        for name in self.mirrors:
            modules.append(
                f'[submodule "{name}"]\n\tpath = {name}\n'
                f'\turl = {self.directory / (Path(name).name + "-unavailable")}\n'
            )
            self.git(
                self.root, "update-index", "--add", "--cacheinfo",
                f"160000,{self.pins[name]},{name}",
            )
        self.modules = "".join(modules)
        (self.root / ".gitmodules").write_text(self.modules)
        self.git(self.root, "add", ".gitmodules")
        self.git(self.root, "commit", "--quiet", "-m", "Fixture submodules")
        shutil.copy(PROJECT_ROOT / "Makefile", self.root / "Makefile")
        shutil.copy(PROJECT_ROOT / "VERSION", self.root / "VERSION")
        (self.root / "python/src/postgamma").mkdir(parents=True)
        (self.root / "buildsys").mkdir()
        shutil.copy(
            PROJECT_ROOT / "buildsys/prepare_sources.py",
            self.root / "buildsys/prepare_sources.py",
        )
        # Record the handoff without starting a PostgreSQL build.
        (self.root / "buildsys/run_upstream_canary.py").write_text(
            "import json, sys\nfrom pathlib import Path\n"
            "Path('canary-arguments.json').write_text(json.dumps(sys.argv[1:]))\n"
        )

    def command(self, *arguments: str, check: bool = True) -> subprocess.CompletedProcess:
        return subprocess.run(
            arguments, cwd=self.directory, env=self.environment, text=True,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=check,
        )

    def git(self, repository: Path, *arguments: str) -> str:
        return self.command("git", "-C", str(repository), *arguments).stdout.strip()

    def initialize(self, repository: Path) -> None:
        self.command("git", "init", "--quiet", "--initial-branch=master", str(repository))
        self.git(repository, "config", "user.name", "Source setup test")
        self.git(repository, "config", "user.email", "source-test@example.invalid")

    def commit(self, repository: Path) -> None:
        self.git(repository, "add", ".")
        self.git(repository, "commit", "--quiet", "-m", "Fixture source")

    def make(self, target: str, *assignments: str) -> subprocess.CompletedProcess:
        return self.command(
            "make", "--no-print-directory", "-C", str(self.root), target,
            f"PYTHON={sys.executable}", *assignments, check=False,
        )

    def initialize_sources(self) -> None:
        result = self.make(
            "source-init",
            f"PG_REPOSITORY={self.mirrors['postgres']}",
            f"PGVECTOR_REPOSITORY={self.mirrors['third_party/pgvector']}",
        )
        self.assertEqual(result.returncode, 0, result.stdout)

    def assert_pins_preserved(self) -> None:
        self.assertEqual((self.root / ".gitmodules").read_text(), self.modules)
        for name, pin in self.pins.items():
            self.assertEqual(self.git(self.root / name, "rev-parse", "HEAD"), pin)
            self.assertEqual(self.git(self.root / name, "status", "--porcelain"), "")

    def test_initialization_populates_both_submodules_and_remembers_mirrors(self) -> None:
        self.initialize_sources()
        result = self.make("source-init")
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assert_pins_preserved()
        for name, mirror in self.mirrors.items():
            self.assertTrue((self.root / name / "source.c").is_file())
            self.assertEqual(
                self.git(self.root, "config", "--get", f"submodule.{name}.url"),
                str(mirror),
            )
            self.assertEqual(self.git(self.root / name, "remote", "get-url", "origin"), str(mirror))

    def test_mirror_change_updates_initialized_and_deinitialized_sources(self) -> None:
        self.initialize_sources()
        replacement = self.directory / "replacement"
        self.command("git", "clone", "--quiet", str(self.mirrors["postgres"]), str(replacement))
        for deinitialized in (False, True):
            with self.subTest(deinitialized=deinitialized):
                if deinitialized:
                    self.git(self.root, "submodule", "deinit", "--", "postgres")
                result = self.make("source-init", f"PG_REPOSITORY={replacement}")
                self.assertEqual(result.returncode, 0, result.stdout)
                self.assertEqual(
                    self.git(self.root / "postgres", "remote", "get-url", "origin"),
                    str(replacement),
                )
                self.assert_pins_preserved()
                replacement = self.mirrors["postgres"]

    def test_canary_entry_fetches_the_requested_branch_and_preserves_pins(self) -> None:
        self.initialize_sources()
        mirror = self.mirrors["postgres"]
        self.git(mirror, "checkout", "--quiet", "-b", "integration/next")
        (mirror / "source.c").write_text("int candidate_source;\n")
        self.commit(mirror)
        candidate = self.git(mirror, "rev-parse", "HEAD")
        for profile in ("daily", "weekly"):
            with self.subTest(profile=profile):
                result = self.make(
                    f"upstream-{profile}", "PG_BRANCH=integration/next", "JOBS=2",
                    "PG_CONFIGURE_ARGS=--with-icu --with-blocksize=16",
                )
                self.assertEqual(result.returncode, 0, result.stdout)
                arguments = json.loads((self.root / "canary-arguments.json").read_text())
                values = dict(zip(arguments[::2], arguments[1::2]))
                self.assertEqual(values["--profile"], profile)
                self.assertEqual(values["--ref"], "origin/integration/next")
                self.assertEqual(values["--jobs"], "2")
                self.assertEqual(values["--stress-seconds"], "3600")
                self.assertEqual(
                    values["--configure-arguments"].split(),
                    ["--with-icu", "--with-blocksize=16", "--enable-debug",
                     "--enable-cassert", "--enable-tap-tests", "--enable-injection-points"],
                )
                self.assertEqual(
                    self.git(self.root / "postgres", "rev-parse", values["--ref"]),
                    candidate,
                )
                self.assert_pins_preserved()

    def test_failed_fetch_does_not_start_a_canary_with_stale_sources(self) -> None:
        self.initialize_sources()
        result = self.make("upstream-weekly", "PG_BRANCH=missing-branch")
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse((self.root / "canary-arguments.json").exists())
        self.assert_pins_preserved()

    def test_default_weekly_entry_initializes_sources_and_enables_full_checks(self) -> None:
        result = self.make(
            "upstream-weekly", "JOBS=2",
            f"PG_REPOSITORY={self.mirrors['postgres']}",
            f"PGVECTOR_REPOSITORY={self.mirrors['third_party/pgvector']}",
        )
        self.assertEqual(result.returncode, 0, result.stdout)
        arguments = json.loads((self.root / "canary-arguments.json").read_text())
        values = dict(zip(arguments[::2], arguments[1::2]))
        self.assertEqual(values["--ref"], "origin/master")
        self.assertEqual(values["--profile"], "weekly")
        self.assertEqual(
            values["--configure-arguments"].split(),
            ["--without-icu", "--enable-debug", "--enable-cassert",
             "--enable-tap-tests", "--enable-injection-points"],
        )
        self.assert_pins_preserved()

    def test_source_initialization_preserves_local_source_edits(self) -> None:
        self.initialize_sources()
        source = self.root / "postgres/source.c"
        source.write_text("int local_change;\n")
        result = self.make("source-init")
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertEqual(source.read_text(), "int local_change;\n")


if __name__ == "__main__":
    unittest.main()
