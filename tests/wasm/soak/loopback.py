#!/usr/bin/env python3
"""Run a native browser and its asset server in a loopback-only Linux namespace."""
import fcntl
import os
from pathlib import Path
import pwd
import shutil
import socket
import struct
import subprocess
import sys


def namespace_command(command, uid, executable=sys.executable, unshare=None, *, privileged=False, gid=None):
    if not command:
        raise ValueError('A browser driver command is required')
    unshare = unshare or shutil.which('unshare')
    if not unshare:
        raise RuntimeError('Native Firefox resource tests require util-linux unshare')
    if privileged:
        if not uid or gid is None:
            raise ValueError('Privileged namespace setup requires the original non-root UID and GID')
        # Some old kernels support netns but not userns. Root configures only
        # loopback; the driver and browser run after dropping back to the caller.
        return ['sudo', '-n', '-E', unshare, '--net', executable,
                str(Path(__file__).resolve()), '--inside-user', str(uid), str(gid), *command]
    # An unprivileged runner maps only its own UID into a new user namespace.
    # A root runner can create the network namespace without a UID mapping.
    return [unshare, *(['--user', '--map-root-user'] if uid else []), '--net',
            executable, str(Path(__file__).resolve()), '--inside', *command]


def enable_loopback():
    names = [line.split(':', 1)[0].strip()
             for line in Path('/proc/self/net/dev').read_text().splitlines()[2:]]
    if names != ['lo']:
        raise RuntimeError('Refuse to configure a namespace with external interfaces')
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as control:
        current = fcntl.ioctl(control.fileno(), 0x8913, struct.pack('16sH14x', b'lo', 0))
        flags = struct.unpack_from('H', current, 16)[0]
        fcntl.ioctl(control.fileno(), 0x8914, struct.pack('16sH14x', b'lo', flags | 1))


def main():
    command = sys.argv[1:]
    if command[:1] in (['--inside'], ['--inside-user']):
        identity = None
        if command[0] == '--inside-user':
            uid, gid = map(int, command[1:3])
            if (os.geteuid() != 0 or uid <= 0 or
                    str(uid) != os.environ.get('SUDO_UID') or str(gid) != os.environ.get('SUDO_GID')):
                raise RuntimeError('Namespace identity must match the invoking user')
            identity = pwd.getpwuid(uid)
            command = command[3:]
        else:
            command = command[1:]
        if not command:
            raise ValueError('A browser driver command is required')
        enable_loopback()
        if identity is not None:
            os.initgroups(identity.pw_name, gid)
            os.setgid(gid)
            os.setuid(uid)
            os.environ.update(USER=identity.pw_name, LOGNAME=identity.pw_name, HOME=identity.pw_dir)
    else:
        uid = os.geteuid()
        isolated = namespace_command(command, uid)
        if uid and subprocess.run(isolated[:4] + ['true'], stdout=subprocess.DEVNULL,
                                  stderr=subprocess.PIPE, timeout=10).returncode:
            isolated = namespace_command(command, uid, privileged=True, gid=os.getegid())
        command = isolated
    os.execvp(command[0], command)


if __name__ == '__main__':
    try:
        main()
    except Exception as error:
        sys.exit('Native Firefox network isolation: ' + str(error))
