"""Tests for the public static SDK release archive."""

from __future__ import annotations

import hashlib
import json
import os
import subprocess
import sys
import tarfile
import tempfile
import unittest
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(PROJECT_ROOT / "buildsys"))

from package_static_sdk import (  # noqa: E402
    StaticSdkPackageError,
    package_static_sdk,
)


class StaticSdkPackageTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.sdk = self.root / "sdk"
        for relative in (
            "include/postgamma/postgamma.h",
            "include/postgamma/postgamma_arrow.h",
            "include/postgamma/postgamma_extension.h",
            "lib/libpostgamma.a",
            "lib/postgamma-static-libs.txt",
        ):
            path = self.sdk / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(f"fixture {relative}\n", encoding="utf-8")
        pkg_config = self.sdk / "lib/pkgconfig/postgamma.pc"
        pkg_config.parent.mkdir(parents=True)
        pkg_config.write_text("Name: PostGamma\nVersion: 1.3\n", encoding="utf-8")
        self.resources = self.root / "resources"
        (self.resources / "bin").mkdir(parents=True)
        (self.resources / "lib").mkdir()
        (self.resources / "share").mkdir()
        postgres = self.resources / "bin/postgres"
        postgres.write_bytes(b"postgres\n")
        postgres.chmod(0o755)
        (self.resources / "share/postgres.bki").write_bytes(b"catalog\n")
        self.example = self.root / "quickstart.c"
        self.example.write_text("int main(void) { return 0; }\n", encoding="utf-8")
        self.project_license = self.root / "LICENSE"
        self.project_license.write_bytes((PROJECT_ROOT / "LICENSE").read_bytes())
        self.project_notice = self.root / "NOTICE"
        self.project_notice.write_bytes((PROJECT_ROOT / "NOTICE").read_bytes())
        self.third_party_notices = self.root / "THIRD_PARTY_NOTICES"
        self.third_party_notices.write_bytes(
            (PROJECT_ROOT / "THIRD_PARTY_NOTICES").read_bytes()
        )
        self.postgresql_license = self.root / "COPYRIGHT"
        self.postgresql_license.write_bytes(
            (PROJECT_ROOT / "licenses/LICENSE.postgresql").read_bytes()
        )
        self.pgvector_license = self.root / "PGVECTOR-LICENSE"
        self.pgvector_license.write_bytes(
            (PROJECT_ROOT / "licenses/LICENSE.pgvector").read_bytes()
        )

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def package(self, name: str) -> tuple[Path, dict[str, object]]:
        output = self.root / f"{name}.tar.gz"
        document = package_static_sdk(
            sdk_root=self.sdk,
            resource_root=self.resources,
            example=self.example,
            project_license=self.project_license,
            project_notice=self.project_notice,
            third_party_notices=self.third_party_notices,
            postgresql_license=self.postgresql_license,
            pgvector_license=self.pgvector_license,
            version="0.1.0a1",
            platform="linux-x86_64",
            source_date_epoch=1724889600,
            output=output,
            checksum=self.root / f"{name}.tar.gz.sha256",
            receipt=self.root / f"{name}.json",
        )
        return output, document

    def test_archive_is_deterministic_complete_and_relocatable(self) -> None:
        first_path, first = self.package("first")
        second_path, second = self.package("second")
        self.assertEqual(first_path.read_bytes(), second_path.read_bytes())
        self.assertEqual(first["archive_sha256"], second["archive_sha256"])
        self.assertEqual(
            first["archive_sha256"],
            hashlib.sha256(first_path.read_bytes()).hexdigest(),
        )
        self.assertEqual(
            (self.root / "first.tar.gz.sha256").read_text(encoding="ascii"),
            f"{first['archive_sha256']}  first.tar.gz\n",
        )
        archive_root = "postgamma-sdk-0.1.0a1-linux-x86_64"
        with tarfile.open(first_path, "r:gz") as archive:
            members = archive.getmembers()
            names = {member.name.rstrip("/") for member in members}
            self.assertIn(f"{archive_root}/lib/libpostgamma.a", names)
            self.assertIn(f"{archive_root}/resource-pack/lib", names)
            self.assertIn(f"{archive_root}/resource-pack/bin/postgres", names)
            self.assertIn(f"{archive_root}/examples/quickstart.c", names)
            self.assertIn(f"{archive_root}/licenses/LICENSE.postgamma", names)
            self.assertIn(f"{archive_root}/licenses/NOTICE", names)
            self.assertIn(f"{archive_root}/licenses/THIRD_PARTY_NOTICES", names)
            self.assertIn(f"{archive_root}/licenses/LICENSE.postgresql", names)
            self.assertIn(f"{archive_root}/licenses/LICENSE.pgvector", names)
            self.assertTrue(
                all(
                    member.name.startswith(f"{archive_root}/")
                    or member.name.rstrip("/") == archive_root
                    for member in members
                )
            )
            self.assertTrue(all(member.mtime == 1724889600 for member in members))
            manifest_stream = archive.extractfile(f"{archive_root}/MANIFEST.json")
            self.assertIsNotNone(manifest_stream)
            assert manifest_stream is not None
            manifest = json.loads(manifest_stream.read())
        self.assertEqual(manifest["kind"], "postgamma.static-sdk-manifest")
        self.assertEqual(manifest["product_version"], "0.1.0a1")
        self.assertEqual(manifest["sdk_abi"], "1.3")
        self.assertEqual(manifest["license_expression"], "Apache-2.0")
        self.assertEqual(manifest["project_url"], "https://postgamma.com")
        self.assertEqual(first["license_expression"], "Apache-2.0")
        self.assertEqual(first["project_url"], "https://postgamma.com")
        self.assertEqual(manifest["payload_tree_sha256"], first["payload_tree_sha256"])
        paths = {entry["path"] for entry in manifest["files"]}
        self.assertIn("resource-pack/share/postgres.bki", paths)
        self.assertNotIn(str(self.root), json.dumps(manifest, sort_keys=True))

    def test_make_fragment_packages_cleanly_and_reuses_only_current_outputs(self) -> None:
        # Real release rules and packager, with small SDK inputs instead of a kernel build.
        source = (PROJECT_ROOT / "Makefile").read_text()
        variables = source[:source.index(".PHONY:")]
        evidence = self.root / "evidence.json"
        evidence.write_text("{}")
        archive = self.root / "dist/postgamma-sdk.tar.gz"
        receipt = self.root / "dist/receipt.json"
        values = {
            "PYTHON": sys.executable,
            "EMBEDDED_KERNEL_BUILD": self.sdk,
            "EMBEDDED_KERNEL_STATIC_SDK_EVIDENCE": evidence,
            "EMBEDDED_RELEASE_RESOURCE_EVIDENCE": evidence,
            "EMBEDDED_RELEASE_RESOURCE_ROOT": self.resources,
            "EMBEDDED_KERNEL_PUBLIC_API_STATIC_LIBRARY": self.sdk / "lib/libpostgamma.a",
            "EMBEDDED_KERNEL_STATIC_LINK_OPTIONS": self.sdk / "lib/postgamma-static-libs.txt",
            "EMBEDDED_KERNEL_STATIC_PKG_CONFIG": self.sdk / "lib/pkgconfig/postgamma.pc",
            "EMBEDDED_KERNEL_STATIC_CORE_HEADER": self.sdk / "include/postgamma/postgamma.h",
            "EMBEDDED_KERNEL_STATIC_ARROW_HEADER": self.sdk / "include/postgamma/postgamma_arrow.h",
            "EMBEDDED_KERNEL_STATIC_EXTENSION_HEADER": self.sdk / "include/postgamma/postgamma_extension.h",
            "EMBEDDED_API_RELEASE_EXAMPLE": self.example,
            "STATIC_SDK_RELEASE_ARCHIVE": archive,
            "STATIC_SDK_RELEASE_CHECKSUM": self.root / "dist/checksum",
            "STATIC_SDK_RELEASE_RECEIPT": receipt,
        }
        fixture = self.root / "fixture.mk"
        fixture.write_text(variables + "\n" + "\n".join(
            f"{key} := {value}" for key, value in values.items()) +
            "\ndoctor-sdk version-check:; @:\ninclude buildsys/make/release.mk\n")
        command = ["make", "--no-print-directory", "-f", str(fixture),
                   f"ROOT={PROJECT_ROOT}", f"BUILD_DIR={self.root / 'build'}", "static-sdk-package"]
        def package():
            subprocess.run(command, cwd=PROJECT_ROOT, check=True,
                           text=True, capture_output=True, timeout=30)
        package()
        first = archive.stat().st_mtime_ns
        first_hash = json.loads(receipt.read_text())["archive_sha256"]
        package()
        self.assertEqual(archive.stat().st_mtime_ns, first)
        self.example.write_text("int main(void) { return 1; }\n")
        os.utime(self.example, ns=(first + 1, first + 1))
        package()
        self.assertNotEqual(json.loads(receipt.read_text())["archive_sha256"], first_hash)
        receipt.unlink()
        package()
        self.assertTrue(receipt.is_file())

    def test_project_license_is_mandatory(self) -> None:
        self.project_license.unlink()
        with self.assertRaisesRegex(StaticSdkPackageError, "LICENSE.postgamma"):
            self.package("missing-license")

    def test_project_license_must_be_canonical_apache_2(self) -> None:
        self.project_license.write_text("not a license\n", encoding="utf-8")
        with self.assertRaisesRegex(StaticSdkPackageError, "canonical Apache-2.0"):
            self.package("wrong-license")

    def test_project_notice_is_mandatory(self) -> None:
        self.project_notice.unlink()
        with self.assertRaisesRegex(StaticSdkPackageError, "licenses/NOTICE"):
            self.package("missing-notice")

    def test_third_party_notices_are_mandatory(self) -> None:
        self.third_party_notices.unlink()
        with self.assertRaisesRegex(
            StaticSdkPackageError, "licenses/THIRD_PARTY_NOTICES"
        ):
            self.package("missing-third-party-notices")

    def test_pgvector_license_must_match_the_pinned_release(self) -> None:
        self.pgvector_license.write_text("not a license\n", encoding="utf-8")
        with self.assertRaisesRegex(
            StaticSdkPackageError, "pinned upstream release"
        ):
            self.package("wrong-pgvector-license")

    def test_postgresql_license_must_match_the_pinned_release(self) -> None:
        self.postgresql_license.write_text("not a license\n", encoding="utf-8")
        with self.assertRaisesRegex(
            StaticSdkPackageError, "PostgreSQL license must match"
        ):
            self.package("wrong-postgresql-license")

    def test_resource_pack_symlinks_are_rejected(self) -> None:
        (self.resources / "share/alias").symlink_to("postgres.bki")
        with self.assertRaisesRegex(StaticSdkPackageError, "symlinks"):
            self.package("symlink")


if __name__ == "__main__":
    unittest.main()
