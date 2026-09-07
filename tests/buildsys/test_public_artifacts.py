"""Tests for release-artifact path isolation."""

from __future__ import annotations

import io
import sys
import tarfile
import tempfile
import unittest
import zipfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "buildsys"))

import check_public_artifacts  # noqa: E402


def write_tar(path: Path, name: str, content: bytes) -> None:
    with tarfile.open(path, "w:gz") as archive:
        info = tarfile.TarInfo(name)
        info.size = len(content)
        archive.addfile(info, io.BytesIO(content))


def write_wheel(path: Path, name: str, content: bytes) -> None:
    with zipfile.ZipFile(path, "w") as archive:
        archive.writestr(name, content)


class PublicArtifactTests(unittest.TestCase):
    def artifacts(self, root: Path) -> tuple[Path, Path]:
        static = root / "postgamma-sdk.tar.gz"
        wheel = root / "postgamma.whl"
        write_tar(static, "postgamma-sdk/lib/libpostgamma.a", b"public-library")
        write_wheel(wheel, "postgamma/__init__.py", b"__version__ = '1.0'")
        return static, wheel

    def test_accepts_semantic_reproducible_artifacts(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            static, wheel = self.artifacts(root)
            report = check_public_artifacts.check(root, static, wheel)
            self.assertEqual(report["status"], "pass")
            self.assertEqual(report["build_path_violations"], 0)

    def test_rejects_the_build_host_source_root_in_binary_strings(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            static, wheel = self.artifacts(root)
            write_wheel(
                wheel,
                "postgamma/_native.so",
                b"debug\x00" + str(root).encode("utf-8") + b"/src/native.c\x00",
            )
            with self.assertRaisesRegex(
                check_public_artifacts.PublicArtifactError,
                "build-host source path",
            ):
                check_public_artifacts.check(root, static, wheel)


if __name__ == "__main__":
    unittest.main()
