"""Private browser ABI selection, dependency checks and cache tamper detection."""
from pathlib import Path
import json
import os
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "buildsys"))
import wasm_browser_runtime as runtime
import wasm_browsers
import wasm_toolchain as tools


class BrowserRuntimeTests(unittest.TestCase):
    def config(self, root):
        profile = ROOT / "manifests/profiles/wasm-emscripten.json"
        return tools.Configuration(root, root / "build", root / "deps", root / "sdk",
                                   profile, tools.read_json(profile), "release", False)

    def test_private_runtime_selection_and_explicit_overrides(self):
        config = self.config(ROOT)
        with mock.patch.object(runtime.platform, "system", return_value="Linux"), \
             mock.patch.object(runtime.platform, "machine", return_value="x86_64"):
            for version, selected in [("2.17", True), ("2.23", True), ("2.34", True), ("2.35", False), ("2.39", False)]:
                with self.subTest(version=version), \
                     mock.patch.object(runtime.platform, "libc_ver", return_value=("glibc", version)):
                    self.assertEqual(runtime.location(config) is not None, selected)
            with mock.patch.object(runtime.platform, "libc_ver", return_value=("glibc", "2.17")):
                config.external_sdk = True
                self.assertIsNone(runtime.location(config))
                config.external_sdk = False
                config.browser_override = "/custom/chrome"
                self.assertIsNone(runtime.location(config))

    def test_browser_environment_keeps_private_libc_out_of_node_and_compilers(self):
        config = self.config(ROOT)
        private = config.deps / "browser-runtime-ubuntu22"
        env = {"PATH": "/usr/bin", "LD_LIBRARY_PATH": "/caller/libs"}
        with mock.patch.object(runtime, "location", return_value=private):
            active = tools.browser_environment(config, env)
            install = runtime.environment(config, env, install=True)
        self.assertEqual(active["LD_LIBRARY_PATH"], "/caller/libs")
        self.assertEqual(active["PLAYWRIGHT_BROWSERS_PATH"], str(private / "browsers"))
        self.assertEqual(install["PLAYWRIGHT_BROWSERS_PATH"], str(private / "raw-browsers"))
        self.assertEqual(install["PATH"], "/usr/bin")
        self.assertEqual(active["PLAYWRIGHT_HOST_PLATFORM_OVERRIDE"], "ubuntu22.04-x64")
        self.assertEqual(env, {"PATH": "/usr/bin", "LD_LIBRARY_PATH": "/caller/libs"})

    def test_elf_code_identity_ignores_metadata_but_detects_code_changes(self):
        # Minimal ELF64: one program header and one executable section.
        data = bytearray(196)
        data[:6] = b"\x7fELF\x02\x01"
        struct.pack_into("<QQ", data, 32, 64, 120)
        struct.pack_into("<HHHH", data, 54, 56, 1, 64, 1)
        struct.pack_into("<I", data, 64, 3)
        struct.pack_into("<IIQQQQ", data, 120, 0, 1, 4, 0, 192, 4)
        data[192:] = b"code"
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "elf"
            path.write_bytes(data)
            original = runtime.elf_metadata(path)
            data[96] = 1  # Loader metadata, outside executable code.
            path.write_bytes(data)
            self.assertEqual(runtime.elf_metadata(path), original)
            data[192] = 1
            path.write_bytes(data)
            self.assertNotEqual(runtime.elf_metadata(path), original)
            struct.pack_into("<I", data, 64, 1)  # Shared library: no interpreter.
            path.write_bytes(data)
            self.assertEqual(runtime.elf_metadata(path), (False, ""))
            path.write_bytes(b"#!/bin/sh\n")
            self.assertEqual(runtime.elf_metadata(path), (False, ""))

    def test_doctor_rejects_missing_and_changed_runtime_without_installing(self):
        with tempfile.TemporaryDirectory() as temporary:
            config = self.config(Path(temporary))
            private = config.deps / "browser-runtime-ubuntu22"
            pin = config.profile["legacy_linux_tools"]["browser_runtime"]
            manifest = config.root / pin["manifest"]
            manifest.parent.mkdir(parents=True)
            manifest.write_text("locked inputs")
            pin["sha256"] = tools.digest(manifest)
            lock = config.package / "package-lock.json"
            browser = config.package / "node_modules/playwright-core/browsers.json"
            browser.parent.mkdir(parents=True)
            lock.write_text("npm lock"); browser.write_text("browser lock")
            with mock.patch.object(runtime, "location", return_value=private), \
                 mock.patch.object(runtime, "setup") as setup:
                with self.assertRaisesRegex(tools.WasmError, "missing"):
                    runtime.identity(config)
                files = {}
                for name in ["root/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2",
                             "root/lib/x86_64-linux-gnu/libc.so.6", "bin/ldd", "fonts.conf",
                             "browsers/firefox/libxul.so"]:
                    path = private / name
                    path.parent.mkdir(parents=True, exist_ok=True)
                    path.write_text(name)
                    files[name] = tools.digest(path)
                state = {"files": files, "manifest_sha256": pin["sha256"],
                         "package_lock_sha256": tools.digest(lock), "browsers_json_sha256": tools.digest(browser),
                         "executables": [{"file": "firefox"}]}
                tools.write_json(private / "receipt.json", state)
                self.assertEqual(runtime.identity(config), state)
                (private / "browsers/firefox/libxul.so").write_text("changed")
                with self.assertRaisesRegex(tools.WasmError, "identity mismatch"):
                    runtime.identity(config)
                setup.assert_not_called()

    def test_loader_preload_warning_is_a_failed_gles_gate(self):
        config = self.config(ROOT)
        engine = {"name": "webkit", "executable": str(config.deps / "webkit/pw_run.sh")}
        def run(command, **kwargs):
            error = "ERROR: libGLESv2.so.2 cannot be preloaded: ignored" if "--preload" in command else ""
            return subprocess.CompletedProcess(command, 0, "", error)
        with mock.patch.object(runtime, "location", return_value=config.deps / "private"), \
             mock.patch.object(wasm_browsers.subprocess, "run", side_effect=run):
            errors = wasm_browsers.missing_libraries(config, [engine], {})
        self.assertEqual(len(errors), 1)
        self.assertIn("libGLESv2.so.2 runtime load failed", errors[0])

    def test_native_firefox_preserves_upstream_and_rejects_changed_copy(self):
        with tempfile.TemporaryDirectory() as temporary:
            config = self.config(Path(temporary))
            local = config.deps / 'browser-runtime-ubuntu22'
            raw = config.deps / 'firefox-native/155.0/bundle'
            executable = raw / 'firefox/firefox'
            executable.parent.mkdir(parents=True)
            executable.write_text('upstream browser')
            policy = raw / 'firefox/distribution/policies.json'
            policy.parent.mkdir(); policy.write_text('update policy')
            original = tools.digest(executable)
            receipt = raw.parent / 'receipt.json'
            tools.write_json(receipt, {'pin': {'version': '155.0'}, 'files': {'firefox/firefox': original},
                'configuration': {'path': 'firefox/distribution/policies.json', 'sha256': tools.digest(policy)}})
            patcher = local / 'tools/tool.data/scripts/patchelf'
            patcher.parent.mkdir(parents=True); patcher.touch()
            tools.write_json(local / 'receipt.json', {'runtime': 'pinned'})
            with mock.patch.object(runtime, 'location', return_value=local), \
                 mock.patch.object(runtime, 'identity', return_value={}):
                adapted, record = runtime.prepare_firefox(config, executable, receipt, config.root / 'test.log')
                self.assertNotEqual(adapted, executable)
                self.assertEqual(tools.digest(executable), original)
                with mock.patch.object(runtime, 'copy_executables') as copy:
                    self.assertEqual(runtime.prepare_firefox(config, executable, receipt, config.root / 'test.log'),
                                     (adapted, record))
                    adapted.write_text('tampered copy')
                    with self.assertRaisesRegex(tools.WasmError, 'identity mismatch'):
                        runtime.prepare_firefox(config, executable, receipt, config.root / 'test.log')
                    copy.assert_not_called()

    def test_native_firefox_on_modern_host_uses_original_executable(self):
        with mock.patch.object(runtime, 'location', return_value=None):
            self.assertEqual(runtime.prepare_firefox(self.config(ROOT), Path('/firefox'), Path('/receipt'), Path('/log')),
                             (Path('/firefox'), None))


if __name__ == "__main__":
    unittest.main()
