"""Exercise the embedded C archive position-independent-code contract."""

from __future__ import annotations

import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parents[2]
EMBEDDED_C_DIR = PROJECT_ROOT / "embedded-c"


@unittest.skipUnless(
    shutil.which("make") and shutil.which("cc"),
    "GNU Make and a C compiler are required",
)
class EmbeddedCMakefileTests(unittest.TestCase):
    def test_archive_object_links_into_a_shared_library(self) -> None:
        make = shutil.which("make")
        cc = shutil.which("cc")
        assert make is not None
        assert cc is not None

        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            build = directory / "build"
            object_path = build / "data_directory_lock.o"
            library_path = directory / "libdata-directory-lock.so"
            compile_result = subprocess.run(
                [
                    make,
                    "--no-print-directory",
                    "-C",
                    str(EMBEDDED_C_DIR),
                    f"BUILD_DIR={build}",
                    "CFLAGS=-O2 -fno-pic -fno-PIE",
                    str(object_path),
                ],
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                check=False,
            )
            self.assertEqual(compile_result.returncode, 0, compile_result.stdout)

            link_result = subprocess.run(
                [
                    cc,
                    "-shared",
                    "-Wl,-z,defs",
                    str(object_path),
                    "-pthread",
                    "-o",
                    str(library_path),
                ],
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                check=False,
            )
            self.assertEqual(link_result.returncode, 0, link_result.stdout)
            self.assertTrue(library_path.is_file())


if __name__ == "__main__":
    unittest.main()
