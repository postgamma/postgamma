"""Runtime probes must consume the selected mode's verified target layouts."""
import json
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'buildsys'))
from wasm_runtime import target_layouts
from wasm_toolchain import WasmError, digest


class RuntimeLayoutTests(unittest.TestCase):
    def test_release_layout_and_tampered_receipt(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary) / 'release'
            include = directory / 'generated/runtime/include/postgamma'
            include.mkdir(parents=True)
            outputs = {}
            for name in ('backend_state_layout.h', 'backend_state_descriptors.inc', 'guc_state_layout.h'):
                path = include / name
                path.write_text('verified target layout')
                outputs[str(path.relative_to(directory))] = digest(path)
            report = directory / 'generation.json'
            report.write_text(json.dumps({'status': 'passed', 'outputs': outputs}))
            (directory / 'reports').mkdir()
            (directory / 'reports/generate.json').write_text(json.dumps({'status': 'passed', 'run_report': str(report)}))
            config = SimpleNamespace(mode='release')
            def prepare(selected, longjmp):
                self.assertIs(selected, config)
                self.assertEqual(selected.mode, 'release')
                return directory, {}, {}, {}
            with patch('wasm_kernel.prepare', side_effect=prepare):
                self.assertEqual(target_layouts(config, 'emscripten')[0], include.parent)
                (include / 'guc_state_layout.h').write_text('stale target layout')
                with self.assertRaisesRegex(WasmError, 'differs from generation receipt'):
                    target_layouts(config, 'emscripten')
