"""Exercise bounded pg_upgrade copies and build-local source ownership."""

from __future__ import annotations

import json
import os
import shlex
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "buildsys"))

import configure_postgres as configure  # noqa: E402
from c_source_probe import function_span, sanitize_c  # noqa: E402


class PgUpgradeCopyRangeTests(unittest.TestCase):
    def test_override_is_tracked_repaired_and_removed_without_editing_source(self):
        original = (ROOT / "postgres" / configure.PG_UPGRADE_FILE).read_text()
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "source"
            upstream = source / configure.PG_UPGRADE_FILE
            upstream.parent.mkdir(parents=True)
            upstream.write_text(original)
            (source / "configure").write_text("#!/bin/sh\n")
            build = root / "build/reference"
            target = build / configure.PG_UPGRADE_FILE

            def run_configure(*args, **kwargs):
                (build / "config.status").touch()
                target.parent.mkdir(parents=True)
                target.symlink_to(upstream)
                return subprocess.CompletedProcess(args[0], 0)

            def run():
                return configure.ensure_configured(source, build, build.parent, [], {})

            with patch.object(configure.subprocess, "run", side_effect=run_configure):
                self.assertTrue(run())
                self.assertFalse(target.is_symlink())
                self.assertEqual(upstream.read_text(), original)
                state = json.loads((build / configure.STATE_FILENAME).read_text())
                self.assertEqual(state["source_overrides"], {
                    configure.PG_UPGRADE_FILE: configure.sha256(target)
                })
                self.assertFalse(run())
                for damage in ("missing", "modified", "symlink"):
                    with self.subTest(damage=damage):
                        target.unlink()
                        if damage == "modified":
                            target.write_text("invalid C\n")
                        elif damage == "symlink":
                            target.symlink_to(upstream)
                        self.assertTrue(run())
                        self.assertFalse(run())
                        self.assertEqual(upstream.read_text(), original)

                # An upstream fix invalidates and removes the previous override.
                fixed = target.read_text() + "\n/* Upstream revision. */\n"
                upstream.write_text(fixed)
                self.assertTrue(run())
                state = json.loads((build / configure.STATE_FILENAME).read_text())
                self.assertEqual(state["source_overrides"], {})
                self.assertEqual(target.read_text(), fixed)
                self.assertEqual(upstream.read_text(), fixed)

    def test_changed_copy_calls_require_review_before_configure_removes_build(self):
        original = (ROOT / "postgres" / configure.PG_UPGRADE_FILE).read_text()
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "source"
            path = source / configure.PG_UPGRADE_FILE
            path.parent.mkdir(parents=True)
            (source / "configure").touch()
            build = root / "build/reference"
            build.mkdir(parents=True)
            marker = build / "keep"
            marker.touch()
            for modified in (
                original.replace("SSIZE_MAX", "PG_INT32_MAX", 1),
                original.replace("NULL, SSIZE_MAX", "NULL,\nSSIZE_MAX"),
            ):
                with self.subTest(source=modified[:20]):
                    path.write_text(modified)
                    with self.assertRaisesRegex(configure.ConfigurationError, "calls changed"):
                        configure.ensure_configured(source, build, build.parent, [], {})
                    self.assertTrue(marker.exists())

    def test_upstream_copy_loop_handles_legacy_overflow_short_copies_and_errors(self):
        compiler = shlex.split(os.environ.get("CC", "cc"))
        if not compiler or shutil.which(compiler[0]) is None:
            self.skipTest("C compiler is unavailable")
        original = (ROOT / "postgres" / configure.PG_UPGRADE_FILE).read_text()
        corrected = configure.pg_upgrade_source_overrides(ROOT / "postgres")[
            configure.PG_UPGRADE_FILE
        ]
        # Run the actual upstream function body.  The syscall stub reproduces
        # Linux 4.15's overflow check before EOF and forces short transfers.
        harness = r'''
#define _POSIX_C_SOURCE 200809L
#define HAVE_COPY_FILE_RANGE 1
#define PG_BINARY 0
#define PG_INT32_MAX INT32_MAX
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
static int pg_file_create_mode = 0600;
static int inject_error;
static _Noreturn void pg_fatal(const char *format, ...)
{
    (void) format;
    fprintf(stderr, "copy error: %d\n", errno);
    exit(1);
}
static ssize_t copy_file_range(int src, off_t *src_pos, int dst,
                               off_t *dst_pos, size_t count, unsigned flags)
{
    unsigned char buffer[257];
    off_t pos = lseek(src, 0, SEEK_CUR);
    ssize_t copied;
    (void) src_pos;
    (void) dst_pos;
    (void) flags;
    if (pos < 0 || (uintmax_t) pos + count > INT64_MAX)
    {
        errno = EINVAL;
        return -1;
    }
    if (inject_error && pos > 0)
    {
        errno = EIO;
        return -1;
    }
    if (count > sizeof(buffer))
        count = sizeof(buffer);
    copied = read(src, buffer, count);
    if (copied > 0 && write(dst, buffer, copied) != copied)
        return -1;
    return copied;
}
void copyFileByRange(const char *src, const char *dst,
                     const char *schemaName, const char *relName)
@BODY@
int main(int argc, char **argv)
{
    if (argc != 4)
        return 2;
    inject_error = atoi(argv[3]);
    copyFileByRange(argv[1], argv[2], "test", "relation");
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            source = directory / "source"
            destination = directory / "destination"
            for name, content in (("upstream", original), ("bounded", corrected)):
                start, end = function_span(sanitize_c(content), "copyFileByRange", 1)
                probe = directory / f"{name}.c"
                probe.write_text(harness.replace("@BODY@", content[start:end]))
                executable = directory / name
                result = subprocess.run(
                    [*compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
                     "-DNDEBUG", str(probe), "-o", str(executable)],
                    text=True, capture_output=True,
                )
                self.assertEqual(result.returncode, 0, result.stderr)
                for data, inject_error in (
                    (b"", False), (b"x" * 16384, False),
                    (bytes(range(256)) * 4097, False), (b"x" * 16384, True),
                ):
                    with self.subTest(version=name, length=len(data), error=inject_error):
                        source.write_bytes(data)
                        destination.unlink(missing_ok=True)
                        result = subprocess.run(
                            [str(executable), str(source), str(destination), str(int(inject_error))],
                            text=True, capture_output=True,
                        )
                        if name == "upstream" and data:
                            self.assertEqual(result.returncode, 1)
                            self.assertIn("copy error: 22", result.stderr)
                        elif inject_error:
                            self.assertEqual(result.returncode, 1)
                            self.assertIn("copy error: 5", result.stderr)
                        else:
                            self.assertEqual(result.returncode, 0, result.stderr)
                            self.assertEqual(destination.read_bytes(), data)


if __name__ == "__main__":
    unittest.main()
