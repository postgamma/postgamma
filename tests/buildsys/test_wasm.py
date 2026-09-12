"""Exercise Wasm build isolation, evidence, and environment failure handling."""

from __future__ import annotations

import copy
from pathlib import Path
import shutil
import sys
import subprocess
import tempfile
import unittest
from unittest import mock
import urllib.error
import urllib.request


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "buildsys"))

import wasm  # noqa: E402
import wasm_toolchain as toolchain  # noqa: E402
import wasm_browsers as browsers  # noqa: E402


class WasmTests(unittest.TestCase):
    def config(self, root: Path) -> toolchain.Configuration:
        profile_path = ROOT / "manifests/profiles/wasm-emscripten.json"
        return toolchain.Configuration(
            root, root / "build", root / ".deps/wasm", root / ".deps/wasm/emsdk/test",
            profile_path, toolchain.read_json(profile_path), "debug", False,
        )

    def test_dependency_overrides_cannot_be_cleaned_with_build_output(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            build = root / "build"
            for deps, sdk in ((build / "cache", root / "sdk"),
                              (root / "deps", build / "sdk")):
                with self.assertRaisesRegex(toolchain.WasmError, "outside BUILD_DIR"):
                    toolchain.validate_directories(root, build, deps, sdk)
            with self.assertRaisesRegex(toolchain.WasmError, "source checkout"):
                toolchain.validate_directories(root, root, root / "deps", root / "sdk")

    def test_symlinked_dependency_cache_is_checked_after_resolution(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "wasm").mkdir()
            (root / "build/cache").mkdir(parents=True)
            (root / "wasm/node_modules").symlink_to(root / "build/cache", target_is_directory=True)
            with self.assertRaisesRegex(toolchain.WasmError, "outside BUILD_DIR"):
                toolchain.validate_directories(root, root / "build", root / "deps", root / "sdk")

    def test_wasm_output_cannot_alias_a_native_directory(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "build/reference").mkdir(parents=True)
            (root / "build/wasm").symlink_to(root / "build/reference", target_is_directory=True)
            with self.assertRaisesRegex(toolchain.WasmError, "symlink"):
                toolchain.validate_directories(root, root / "build", root / "deps", root / "sdk")

    def test_configuration_changes_do_not_reuse_the_same_target_directory(self):
        config = self.config(ROOT)
        tools = {key: "initial" for key in ("emcc_version", "emcc_sha256", "clang_version",
                                           "clang_sha256", "sdk_config_sha256",
                                           "wasm_ld_sha256", "wasm_opt_sha256")}
        original = toolchain.fingerprint(wasm.build_configuration(config, tools))
        changed = copy.deepcopy(config)
        changed.mode = "release"
        self.assertNotEqual(original, toolchain.fingerprint(wasm.build_configuration(changed, tools)))
        changed = copy.deepcopy(config)
        changed.profile["link_flags"].append("-sSUPPORT_LONGJMP=wasm")
        self.assertNotEqual(original, toolchain.fingerprint(wasm.build_configuration(changed, tools)))
        tools["clang_sha256"] = "different compiler"
        self.assertNotEqual(original, toolchain.fingerprint(wasm.build_configuration(config, tools)))

    def test_changed_source_and_corrupt_or_missing_output_invalidate_reuse(self):
        with tempfile.TemporaryDirectory() as temporary:
            site = Path(temporary)
            artifact = site / "smoke.wasm"
            artifact.write_bytes(b"first module")
            inputs = {"source_sha256": "first source"}
            receipt = {"inputs": inputs, "outputs": {artifact.name: toolchain.digest(artifact)}}
            self.assertTrue(wasm.outputs_match(receipt, inputs, site))
            self.assertFalse(wasm.outputs_match(receipt, {"source_sha256": "new source"}, site))
            artifact.write_bytes(b"corrupt module")
            self.assertFalse(wasm.outputs_match(receipt, inputs, site))
            artifact.unlink()
            self.assertFalse(wasm.outputs_match(receipt, inputs, site))
            self.assertFalse(wasm.outputs_match({"inputs": inputs}, inputs, site))

    def test_missing_sdk_fails_before_probe_build_and_retains_each_failure(self):
        with tempfile.TemporaryDirectory() as temporary:
            config = self.config(Path(temporary))
            for _ in range(2):
                with self.assertRaisesRegex(toolchain.WasmError, "make wasm-setup"):
                    wasm.probe(config, check=True)
            reports = list((config.build / "wasm/doctor").glob("*/report.json"))
            self.assertEqual(len(reports), 2)
            self.assertTrue(all(toolchain.read_json(path)["status"] == "failed" for path in reports))
            self.assertFalse((config.build / "wasm" / config.target).exists())
            self.assertFalse(config.deps.exists())

    def test_secondary_translation_units_and_headers_invalidate_probe_reuse(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            sources = root / "tests/wasm/probes"
            shutil.copytree(ROOT / "tests/wasm/probes", sources)
            artifact = root / "platform.wasm"
            artifact.write_bytes(b"module")
            expected = wasm.probe_sources(root, "platform")
            receipt = {"inputs": expected, "outputs": {artifact.name: toolchain.digest(artifact)}}
            self.assertTrue(wasm.outputs_match(receipt, expected, root))
            for filename in ("platform.h", "platform-libc.c", "platform-threads.c", "stack-overflow.c"):
                path = sources / filename
                original = path.read_text()
                path.write_text(original + "\n/* changed input */\n")
                self.assertFalse(wasm.outputs_match(receipt, wasm.probe_sources(root, "platform"), root))
                path.write_text(original)

    @unittest.skipUnless((ROOT / "postgres/src/include/utils/elog.h").is_file(), "PostgreSQL source not initialized")
    def test_target_recovery_header_is_required_and_invalidates_probe_reuse(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            shutil.copytree(ROOT / "tests/wasm/probes", root / "tests/wasm/probes")
            with self.assertRaisesRegex(toolchain.WasmError, "make source-init"):
                wasm.probe_sources(root, "recovery")
            target = root / "postgres/src/include/utils/elog.h"
            target.parent.mkdir(parents=True)
            source = (ROOT / "postgres/src/include/utils/elog.h").read_text()
            target.write_text(source)
            initial = wasm.probe_sources(root, "recovery")
            target.write_text(source.replace("#define PG_CATCH", "/* changed target */\n#define PG_CATCH"))
            self.assertNotEqual(initial, wasm.probe_sources(root, "recovery"))

    def test_opfs_observer_changes_invalidate_compiled_probe(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            shutil.copytree(ROOT / "tests/wasm/probes", root / "tests/wasm/probes")
            original = wasm.probe_sources(root, "opfs")
            observer = root / "tests/wasm/probes/opfs-observe.js"
            observer.write_text(observer.read_text() + "\n/* changed browser instrumentation */\n")
            self.assertNotEqual(original, wasm.probe_sources(root, "opfs"))
            observer.unlink()
            with self.assertRaises(FileNotFoundError):
                wasm.probe_sources(root, "opfs")

    @unittest.skipUnless((ROOT / "postgres/src/include/utils/elog.h").is_file(), "PostgreSQL source not initialized")
    def test_recovery_extraction_preserves_target_macros_and_rejects_upstream_drift(self):
        source = (ROOT / "postgres/src/include/utils/elog.h").read_text()
        extracted = wasm.recovery_macros(source)
        original = source[source.index("#define PG_TRY("):source.index("extern PGDLLIMPORT sigjmp_buf *PG_exception_stack;")]
        self.assertIn(original, extracted)
        self.assertNotIn("PGDLLIMPORT", extracted)
        for changed in (source.replace("#define PG_TRY(", "#define PG_TRY_CHANGED("),
                        source.replace("#define PG_FINALLY(", "#define PG_FINALLY_CHANGED(")):
            with self.assertRaises(toolchain.WasmError):
                wasm.recovery_macros(changed)

    def test_failed_platform_probe_retains_partial_browser_evidence(self):
        with tempfile.TemporaryDirectory() as temporary:
            config = self.config(ROOT)
            config.build = Path(temporary)
            selected = {"test": "partial-evidence"}
            directory = config.build / "wasm" / config.target / config.mode / toolchain.fingerprint(selected)
            site = directory / "probes/platform"
            site.mkdir(parents=True)
            artifact = site / "platform.wasm"
            artifact.write_bytes(b"module")
            toolchain.write_json(site / "build.json", {
                "inputs": {"configuration": selected, "sources": wasm.probe_sources(ROOT, "platform")},
                "outputs": {artifact.name: toolchain.digest(artifact)},
            })

            def fail_browser(config, tools, env, run_directory, url):
                toolchain.write_json(run_directory / "browser.json", {
                    "status": "failed", "error": "timed out", "events": [{"ready": True}],
                })
                raise toolchain.WasmError("timed out")

            with mock.patch.object(wasm, "doctor", return_value=({"node": "node"}, {}, Path("doctor.json"))), \
                 mock.patch.object(wasm, "build_configuration", return_value=selected), \
                 mock.patch.object(wasm, "source_identity", return_value={}), \
                 mock.patch.object(wasm, "run"), \
                 mock.patch.object(wasm, "run_browser", side_effect=fail_browser):
                with self.assertRaisesRegex(toolchain.WasmError, "timed out"):
                    wasm.probe(config, check=True, name="platform")
            latest = toolchain.read_json(directory / "reports/platform.json")
            report = toolchain.read_json(Path(latest["run_report"]))
            self.assertEqual(latest["status"], "failed")
            self.assertFalse(report["browser_probe_validated"])
            self.assertEqual(report["browser"]["events"], [{"ready": True}])

    def test_failed_browser_details_are_visible_to_the_caller(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            config = self.config(directory)
            toolchain.write_json(directory / "browser.json", {"status": "failed", "error": "libexample.so missing"})
            with mock.patch.object(wasm, "run", side_effect=toolchain.WasmError("command failed")):
                with self.assertRaisesRegex(toolchain.WasmError, "libexample.so missing"):
                    wasm.run_browser(config, {"node": "node"}, {}, directory)

    def test_http_assets_have_required_headers_and_missing_asset_is_an_error(self):
        with tempfile.TemporaryDirectory() as temporary:
            site = Path(temporary)
            (site / "smoke.html").write_text("probe")
            (site / "smoke.mjs").write_text("export default 1;")
            (site / "smoke.wasm").write_bytes(b"\0asm")
            with wasm.serve(site) as url:
                for name, mime in (("smoke.html", "text/html"), ("smoke.mjs", "text/javascript"),
                                   ("smoke.wasm", "application/wasm")):
                    with urllib.request.urlopen(url.rsplit("/", 1)[0] + "/" + name) as response:
                        self.assertEqual(response.headers["Cross-Origin-Opener-Policy"], "same-origin")
                        self.assertEqual(response.headers["Cross-Origin-Embedder-Policy"], "require-corp")
                        self.assertIn(mime, response.headers["Content-Type"])
                with self.assertRaises(urllib.error.HTTPError) as raised:
                    urllib.request.urlopen(url + ".missing")
                self.assertEqual(raised.exception.code, 404)

    def test_wasm_clean_retains_native_output_and_installed_tools(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            config = self.config(root)
            (root / "buildsys").mkdir()
            (root / "buildsys/reset_directory.py").symlink_to(ROOT / "buildsys/reset_directory.py")
            for name in ("build/wasm/probe", "build/reference/native.o", ".deps/wasm/sdk",
                         "wasm/node_modules/package"):
                path = root / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text("retain")
            wasm.clean(config)
            self.assertFalse((root / "build/wasm").exists())
            for name in ("build/reference/native.o", ".deps/wasm/sdk", "wasm/node_modules/package"):
                self.assertEqual((root / name).read_text(), "retain")

    def test_apt_plan_pins_versions_and_refuses_private_libc_replacement(self):
        self.assertEqual(toolchain.apt_download_plan(
            "Inst libexample (1:2.0-1 Ubuntu:24.04 [amd64])\n"
            "Inst libsecond [1.0] (2.0 Ubuntu:24.04 [amd64])\n"
            "Conf libexample (1:2.0-1 Ubuntu:24.04 [amd64])\n"
        ), ["libexample=1:2.0-1", "libsecond=2.0"])
        with self.assertRaisesRegex(toolchain.WasmError, "host libc"):
            toolchain.apt_download_plan("Inst libc6 (2.39 Ubuntu:24.04 [amd64])")
        with self.assertRaisesRegex(toolchain.WasmError, "unrecognized"):
            toolchain.apt_download_plan("Inst an unexpected format")

    def test_browser_library_path_is_not_added_to_compiler_environment(self):
        with tempfile.TemporaryDirectory() as temporary:
            config = self.config(Path(temporary))
            local = config.deps / "browser-libs/root/usr/lib/x86_64-linux-gnu"
            local.mkdir(parents=True)
            env = {"PATH": "/usr/bin", "LD_LIBRARY_PATH": "/opt/host-libs"}
            browser = toolchain.browser_environment(config, env)
            self.assertEqual(env["LD_LIBRARY_PATH"], "/opt/host-libs")
            self.assertEqual(browser["LD_LIBRARY_PATH"], str(local) + ":/opt/host-libs")

    def test_browser_matrix_link_check_retains_symbol_version_failures(self):
        config = self.config(ROOT)
        engine = {"name": "firefox", "executable": "/cached firefox/firefox"}
        failure = subprocess.CompletedProcess([], 1, "", "libxul.so: version NSS_3.95 not found")
        with mock.patch.object(browsers.subprocess, "run", return_value=failure) as run:
            errors = browsers.missing_libraries(config, [engine], {})
            self.assertEqual(errors, ["firefox: libxul.so: version NSS_3.95 not found"])
            self.assertEqual(run.call_count, 2)
            for call in run.call_args_list:
                self.assertTrue(call.kwargs["env"]["LD_LIBRARY_PATH"].startswith("/cached firefox:"))

    def test_webkit_launcher_preserves_cached_libs_and_runtime_driver_lookup(self):
        with tempfile.TemporaryDirectory(prefix="wasm test '") as temporary:
            config = self.config(Path(temporary))
            host = config.deps / "browser-libs/root"
            drivers = host / "usr/lib/x86_64-linux-gnu/dri"
            drivers.mkdir(parents=True)
            vendor = host / "usr/share/glvnd/egl_vendor.d/50_mesa.json"
            vendor.parent.mkdir(parents=True)
            vendor.write_text("{}")
            engine = {"name": "webkit", "executable": str(config.deps / "webkit/pw_run.sh")}
            binary = config.deps / "webkit/minibrowser-wpe/bin/MiniBrowser"
            binary.parent.mkdir(parents=True)
            binary.write_text("#!/bin/sh\nprintf '%s\\n' \"$LD_LIBRARY_PATH\" "
                              "\"$__EGL_VENDOR_LIBRARY_FILENAMES\" \"$LIBGL_DRIVERS_PATH\" \"$1\"\n")
            binary.chmod(0o755)
            launcher = browsers.write_webkit_launcher(config, engine)
            result = subprocess.run([str(launcher), "argument with spaces"], check=True, capture_output=True, text=True)
            lines = result.stdout.splitlines()
            self.assertIn(str(drivers.parent), lines[0])
            self.assertIn(str(binary.parent.parent / "sys/lib"), lines[0])
            self.assertEqual(lines[1:], [str(vendor), str(drivers), "argument with spaces"])

    def test_command_timeout_retains_diagnostics(self):
        with tempfile.TemporaryDirectory() as temporary:
            log = Path(temporary) / "run.log"
            with self.assertRaises(subprocess.TimeoutExpired):
                toolchain.run([sys.executable, "-c", "import time; print('started', flush=True); time.sleep(10)"],
                              log, timeout=0.2)
            self.assertIn("started", log.read_text())

    def test_kernel_commands_dispatch_to_the_startup_driver(self):
        for target, action in (("wasm", "build"), ("wasm-check", "check"), ("wasm-serve", "serve")):
            result = subprocess.run(["make", "-n", "--no-print-directory", target], cwd=ROOT,
                                    text=True, capture_output=True, timeout=10, check=True)
            self.assertIn(f'/wasm_startup.py" {action} ', result.stdout)

    def test_full_suite_dispatches_to_the_weekly_driver(self):
        result = subprocess.run(["make", "-n", "--no-print-directory", "wasm-check-full"], cwd=ROOT,
                                text=True, capture_output=True, timeout=10, check=True)
        self.assertIn('/wasm_ci.py"', result.stdout)
        self.assertIn('--suite weekly', result.stdout)


if __name__ == "__main__":
    unittest.main()
