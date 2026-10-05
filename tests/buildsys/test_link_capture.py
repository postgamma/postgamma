"""Tests for exact compiler-link capture and deterministic merging."""

from __future__ import annotations

import json
import sys
import tempfile
import unittest
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(PROJECT_ROOT / "buildsys"))

import cc_capture  # noqa: E402
import merge_link_db  # noqa: E402


class LinkCaptureTests(unittest.TestCase):
    def test_detects_links_but_not_compile_or_preprocess_commands(self) -> None:
        directory = Path("/tmp/postgamma-link-fixture")
        self.assertEqual(
            cc_capture.link_output(["one.o", "-o", "app"], directory),
            (directory / "app").resolve(),
        )
        self.assertIsNone(
            cc_capture.link_output(["-c", "one.c", "-o", "one.o"], directory)
        )
        self.assertIsNone(
            cc_capture.link_output(["-E", "one.c", "-o", "one.i"], directory)
        )

    def test_link_fragment_has_a_separate_strict_schema(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            destination = Path(temporary) / "fragments"
            directory = Path(temporary) / "build"
            directory.mkdir()
            cc_capture.record_link(
                destination,
                "cc",
                ["one.o", "-lm", "-o", "app"],
                directory,
            )
            fragments = list(destination.glob("*.json"))
            self.assertEqual(len(fragments), 1)
            document = json.loads(fragments[0].read_text(encoding="utf-8"))
        self.assertEqual(document["kind"], cc_capture.LINK_FRAGMENT_KIND)
        self.assertEqual(document["arguments"][0], "cc")
        self.assertEqual(set(document), {
            "schema_version", "kind", "directory", "output", "arguments"
        })


class LinkDatabaseTests(unittest.TestCase):
    @staticmethod
    def write(path: Path, document: dict[str, object]) -> None:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(document, sort_keys=True) + "\n", encoding="utf-8")

    def fixture(
        self, root: Path, libraries: list[str] | None = None
    ) -> tuple[Path, Path, Path, Path]:
        build = root / "reference"
        backend = build / "src/backend"
        backend.mkdir(parents=True)
        main = backend / "main/main.o"
        worker = backend / "worker.o"
        archive = build / "src/common/libpgcommon_srv.a"
        for path in (main, worker, archive):
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(path.name.encode("ascii"))
        configure = build / ".postgamma-configure.json"
        self.write(configure, {"schema_version": 1, "arguments": [], "environment": {}})
        config_log = build / "config.log"
        config_log.write_text("fixture compiler 1.0\n", encoding="utf-8")
        fragments = root / "fragments"
        fragments.mkdir()
        arguments = [
            "cc",
            str(main),
            str(worker),
            str(archive),
            *(libraries or ["-L/usr/lib", "-lm"]),
            "-o",
            str(backend / "postgres"),
        ]
        self.write(
            fragments / "one.json",
            {
                "schema_version": 1,
                "kind": cc_capture.LINK_FRAGMENT_KIND,
                "directory": str(backend),
                "output": str(backend / "postgres"),
                "arguments": arguments,
            },
        )
        return build, configure, config_log, fragments

    def test_merges_and_classifies_exact_backend_link_facts(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            build, configure, config_log, fragments = self.fixture(Path(temporary))
            duplicate = fragments / "duplicate.json"
            duplicate.write_bytes((fragments / "one.json").read_bytes())
            database = merge_link_db.merge_fragments(
                fragments, build, configure, config_log
            )
        self.assertEqual(len(database["commands"]), 1)
        command = database["commands"][0]
        self.assertEqual(command["target"], "postgres-backend")
        self.assertEqual(
            [item["kind"] for item in command["ordered_inputs"]],
            ["object", "object", "archive"],
        )
        self.assertEqual(command["library_arguments"], ["-L/usr/lib", "-lm"])

    def test_rejects_two_different_commands_for_one_output(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            build, configure, config_log, fragments = self.fixture(Path(temporary))
            changed = json.loads((fragments / "one.json").read_text(encoding="utf-8"))
            changed["arguments"].insert(-2, "-pthread")
            self.write(fragments / "two.json", changed)
            with self.assertRaisesRegex(
                merge_link_db.LinkDatabaseError, "ambiguous link commands"
            ):
                merge_link_db.merge_fragments(
                    fragments, build, configure, config_log
                )

    def test_rejects_unknown_fragment_fields(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            build, configure, config_log, fragments = self.fixture(Path(temporary))
            document = json.loads((fragments / "one.json").read_text(encoding="utf-8"))
            document["environment"] = {"SECRET": "must-not-be-captured"}
            self.write(fragments / "one.json", document)
            with self.assertRaisesRegex(
                merge_link_db.LinkDatabaseError, "unknown field"
            ):
                merge_link_db.merge_fragments(
                    fragments, build, configure, config_log
                )


if __name__ == "__main__":
    unittest.main()
