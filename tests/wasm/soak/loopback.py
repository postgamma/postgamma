#!/usr/bin/env python3
"""Run a native browser and its asset server in a loopback-only Linux namespace."""
import fcntl
import os
from pathlib import Path
import shutil
import socket
import struct
import sys


def namespace_command(command, uid, executable=sys.executable, unshare=None):
    if not command:
        raise ValueError('A browser driver command is required')
    unshare = unshare or shutil.which('unshare')
    if not unshare:
        raise RuntimeError('Native Firefox resource tests require util-linux unshare')
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
    if command[:1] == ['--inside']:
        command = command[1:]
        if not command:
            raise ValueError('A browser driver command is required')
        enable_loopback()
    else:
        command = namespace_command(command, os.geteuid())
    os.execvp(command[0], command)


if __name__ == '__main__':
    try:
        main()
    except Exception as error:
        sys.exit('Native Firefox network isolation: ' + str(error))
