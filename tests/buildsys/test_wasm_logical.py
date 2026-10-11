"""Invocation cleanup must preserve allocator and foreign-pointer ownership."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class LogicalAllocationTests(unittest.TestCase):
    @unittest.skipUnless(shutil.which('cc'), 'requires a native C compiler')
    def test_normal_failure_and_foreign_allocations_under_sanitizers(self):
        with tempfile.TemporaryDirectory(prefix='logical-alloc-') as temporary:
            binary = Path(temporary) / 'alloc'
            subprocess.run(['cc', '-std=c11', '-D_GNU_SOURCE', '-g', '-O1', '-fno-omit-frame-pointer',
                            '-fsanitize=address,undefined', '-I' + str(ROOT),
                            str(ROOT / 'wasm/bridge/logical_alloc.c'),
                            str(ROOT / 'tests/wasm/fixtures/logical-alloc.c'), '-o', str(binary)],
                           check=True, capture_output=True, timeout=30)
            result = subprocess.run([str(binary)], capture_output=True, timeout=30,
                                    env=dict(os.environ, ASAN_OPTIONS='detect_leaks=1:abort_on_error=1'))
            self.assertEqual(result.returncode, 0, result.stderr.decode())


if __name__ == '__main__': unittest.main()
