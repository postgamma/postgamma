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

    def test_old_kernel_setup_keeps_netns_and_records_nonroot_identity(self):
        command = loopback.namespace_command(['/node', 'space in argument'], 1001,
            executable='/python', unshare='/unshare', privileged=True, gid=1002)
        self.assertEqual(command[:6], ['sudo', '-n', '-E', '/unshare', '--net', '/python'])
        self.assertEqual(command[-5:], ['--inside-user', '1001', '1002', '/node', 'space in argument'])
        with self.assertRaises(ValueError):
            loopback.namespace_command(['/node'], 0, unshare='/unshare', privileged=True, gid=0)

    def test_privileged_namespace_drops_identity_before_exec(self):
        from types import SimpleNamespace
        calls = []
        with patch.object(loopback.sys, 'argv', ['loopback', '--inside-user', '1001', '1002', '/node']), \
                patch.dict(loopback.os.environ, SUDO_UID='1001', SUDO_GID='1002'), \
                patch.object(loopback.os, 'geteuid', return_value=0), \
                patch.object(loopback.pwd, 'getpwuid', return_value=SimpleNamespace(pw_name='test', pw_dir='/home/test')), \
                patch.object(loopback, 'enable_loopback', side_effect=lambda: calls.append('loopback')), \
                patch.object(loopback.os, 'initgroups', side_effect=lambda *args: calls.append(('groups', args))), \
                patch.object(loopback.os, 'setgid', side_effect=lambda gid: calls.append(('gid', gid))), \
                patch.object(loopback.os, 'setuid', side_effect=lambda uid: calls.append(('uid', uid))), \
                patch.object(loopback.os, 'execvp', side_effect=lambda *args: calls.append(('exec', args))):
            loopback.main()
        self.assertEqual(calls, ['loopback', ('groups', ('test', 1002)), ('gid', 1002),
                                 ('uid', 1001), ('exec', ('/node', ['/node']))])

    def test_privileged_setup_rejects_a_different_user(self):
        with patch.object(loopback.sys, 'argv', ['loopback', '--inside-user', '1001', '1002', '/node']), \
                patch.dict(loopback.os.environ, SUDO_UID='2000', SUDO_GID='1002'), \
                patch.object(loopback.os, 'geteuid', return_value=0), \
                patch.object(loopback, 'enable_loopback') as setup, self.assertRaises(RuntimeError):
            loopback.main()
        setup.assert_not_called()

    def test_external_interface_is_rejected_before_any_interface_change(self):
        source = 'header\nheader\n lo: 0\n eth0: 0\n'
        with patch.object(loopback.Path, 'read_text', return_value=source), \
                patch.object(loopback.socket, 'socket') as socket, self.assertRaises(RuntimeError):
            loopback.enable_loopback()
        socket.assert_not_called()


if __name__ == '__main__':
    unittest.main()
