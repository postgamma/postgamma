"""Resource isolation must not silently use the host's network namespace."""
import importlib.util
from pathlib import Path
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('wasm_loopback', ROOT / 'tests/wasm/soak/loopback.py')
loopback = importlib.util.module_from_spec(spec)
spec.loader.exec_module(loopback)


class LoopbackTests(unittest.TestCase):
    def test_root_creates_a_separate_network_namespace(self):
        command = loopback.namespace_command(['/node', '/driver', 'argument with spaces'], 0,
                                             executable='/python', unshare='/unshare')
        self.assertEqual(command[:3], ['/unshare', '--net', '/python'])
        self.assertEqual(command[-4:], ['--inside', '/node', '/driver', 'argument with spaces'])

    def test_unprivileged_runner_maps_only_its_own_user(self):
        command = loopback.namespace_command(['/node'], 1000, unshare='/unshare')
        self.assertEqual(command[:4], ['/unshare', '--user', '--map-root-user', '--net'])
        self.assertNotIn('sudo', command)

    def test_missing_namespace_support_never_falls_back_to_host_network(self):
        with patch.object(loopback.shutil, 'which', return_value=None), self.assertRaises(RuntimeError):
            loopback.namespace_command(['/node'], 1000)
        with self.assertRaises(ValueError):
            loopback.namespace_command([], 0)

    def test_external_interface_is_rejected_before_any_interface_change(self):
        source = 'header\nheader\n lo: 0\n eth0: 0\n'
        with patch.object(loopback.Path, 'read_text', return_value=source), \
                patch.object(loopback.socket, 'socket') as socket, self.assertRaises(RuntimeError):
            loopback.enable_loopback()
        socket.assert_not_called()


if __name__ == '__main__':
    unittest.main()
