"""Build doctors must not require browsers; browser doctors must not skip engines."""

from __future__ import annotations

import copy
import json
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "buildsys"))
import wasm
import wasm_browsers
import wasm_toolchain


class WasmDoctorTests(unittest.TestCase):
    def config(self, root):
        return SimpleNamespace(root=root, build=root / "build", deps=root / "deps",
                               sdk=root / "sdk", external_sdk=True, browser_override="",
                               profile={"emsdk": {"version": "test"}}, target="wasm32", mode="release")

    def test_toolchain_setup_does_not_download_or_inspect_browsers(self):
        with tempfile.TemporaryDirectory() as temporary:
            config = self.config(Path(temporary))
            with mock.patch.object(wasm_toolchain, "sdk_tools", return_value=({}, {})), \
                 mock.patch.object(wasm_toolchain, "package_ready", return_value=True), \
                 mock.patch.object(wasm_toolchain, "browser_description") as description, \
                 mock.patch.object(wasm_toolchain, "setup_browser_libraries") as libraries, \
                 mock.patch.object(wasm_toolchain, "run") as run:
                wasm_toolchain.setup(config, browser=False)
                description.assert_not_called()
                libraries.assert_not_called()
                run.assert_not_called()
            report = json.loads((config.deps / "receipts/build-tools.json").read_text())
            self.assertEqual(report["status"], "installed")
            self.assertFalse(report["browser_required"])
            self.assertFalse((config.deps / "receipts/tools.json").exists())

    def test_build_doctor_does_not_launch_or_require_a_browser(self):
        with tempfile.TemporaryDirectory() as temporary:
            config = self.config(Path(temporary))
            tools = {"emcc_version": "test", "node_version": "v22"}
            with mock.patch.object(wasm, "sdk_tools", return_value=(tools, {})), \
                 mock.patch.object(wasm, "package_ready", return_value=True), \
                 mock.patch("check_wasm_sdk_contract.check", return_value={}), \
                 mock.patch("wasm_storage_protocol.check", return_value={}), \
                 mock.patch.object(wasm, "browser_description") as description, \
                 mock.patch.object(wasm, "run_browser") as launch, \
                 mock.patch.object(wasm, "setup") as setup:
                _, _, path = wasm.doctor(config, browser=False)
                description.assert_not_called()
                launch.assert_not_called()
                setup.assert_not_called()
            self.assertEqual(json.loads(path.read_text())["status"], "passed")

    def test_build_doctor_reports_missing_sdk_without_installing_it(self):
        with tempfile.TemporaryDirectory() as temporary:
            config = self.config(Path(temporary))
            with mock.patch.object(wasm, "sdk_tools", side_effect=wasm_toolchain.WasmError("missing SDK")), \
                 mock.patch.object(wasm, "setup") as setup:
                with self.assertRaisesRegex(wasm_toolchain.WasmError, "missing SDK"):
                    wasm.doctor(config, browser=False)
                setup.assert_not_called()
            path = next((config.build / "wasm/toolchain-doctor").glob("*/report.json"))
            self.assertEqual(json.loads(path.read_text())["status"], "failed")

    def run_browser_doctor(self, root, *, absent="", missing=(), startup=None, ready=True):
        config = self.config(root)
        info = {"engines": []}
        for name in ("chromium", "firefox", "webkit"):
            binary = root / name
            if name != absent:
                binary.touch()
            info["engines"].append({"name": name, "executable": str(binary)})
        if startup is None:
            startup = {"status": "passed", "engines": [
                {"name": row["name"], "status": "passed", "version": "test"} for row in info["engines"]]}

        def launch(command, *args, **kwargs):
            Path(command[command.index("--output") + 1]).write_text(json.dumps(startup))

        with mock.patch.object(wasm_browsers, "sdk_tools", return_value=({"node": "node"}, {})), \
             mock.patch.object(wasm_browsers, "package_ready", return_value=ready), \
             mock.patch.object(wasm_browsers, "description", return_value=info), \
             mock.patch.object(wasm_browsers.platform, "system", return_value="Linux"), \
             mock.patch.object(wasm_browsers, "missing_libraries", return_value=list(missing)), \
             mock.patch.object(wasm_browsers, "run", side_effect=launch) as run, \
             mock.patch.object(wasm_browsers, "setup_browsers") as setup:
            try:
                path = wasm_browsers.check_browsers(config)
            finally:
                setup.assert_not_called()
                if absent or missing or not ready:
                    run.assert_not_called()
            return json.loads(path.read_text()), run.call_args

    def test_browser_doctor_requires_all_three_installed_browsers(self):
        with tempfile.TemporaryDirectory() as temporary:
            with self.assertRaisesRegex(wasm_toolchain.WasmError, "missing browsers: firefox"):
                self.run_browser_doctor(Path(temporary), absent="firefox")

    def test_browser_doctor_rejects_stale_dependencies_and_missing_libraries(self):
        for options, message in (({"ready": False}, "locked npm dependencies"),
                                 ({"missing": ["firefox: GLIBC_2.28 not found"]}, "GLIBC_2.28")):
            with self.subTest(options=options), tempfile.TemporaryDirectory() as temporary:
                with self.assertRaisesRegex(wasm_toolchain.WasmError, message):
                    self.run_browser_doctor(Path(temporary), **options)

    def test_browser_doctor_launches_installed_engines_without_building_a_kernel(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            launcher = root / "deps/browser-libs/launch-webkit.sh"
            launcher.parent.mkdir(parents=True)
            launcher.touch()
            report, call = self.run_browser_doctor(root)
            self.assertEqual(report["status"], "passed")
            self.assertEqual(len(report["startup"]["engines"]), 3)
            self.assertIn(str(launcher), call.args[0])
            self.assertEqual(call.kwargs["timeout"], 90)

    def test_browser_doctor_rejects_incomplete_duplicate_or_failed_engine_results(self):
        complete = [{"name": name, "status": "passed", "version": "test"}
                    for name in ("chromium", "firefox", "webkit")]
        failed = copy.deepcopy(complete)
        failed[1]["status"] = "failed"
        for rows in (complete[:1], complete[:2] + [complete[0]], failed):
            with self.subTest(rows=rows), tempfile.TemporaryDirectory() as temporary:
                with self.assertRaisesRegex(wasm_toolchain.WasmError, "incomplete or failed"):
                    self.run_browser_doctor(Path(temporary), startup={"status": "passed", "engines": rows})


if __name__ == "__main__":
    unittest.main()
