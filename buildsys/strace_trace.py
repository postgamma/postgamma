#!/usr/bin/env python3
"""Helpers for interpreting strace output from concurrent programs."""

from __future__ import annotations

import re
import subprocess
from collections.abc import Sequence


def strace_command_prefix(strace: str) -> list[str]:
    """Enable seccomp filtering only when the installed strace supports it."""

    try:
        completed = subprocess.run(
            [strace, "--seccomp-bpf", "-V"],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            check=False,
            timeout=10.0,
        )
    except (OSError, subprocess.TimeoutExpired):
        return [strace]
    if completed.returncode == 0:
        return [strace, "--seccomp-bpf"]
    return [strace]


def reassemble_syscall_lines(trace: str, names: Sequence[str]) -> list[str]:
    """Return selected syscall attempts with split strace records reassembled."""

    if not names:
        return []
    alternatives = "|".join(
        sorted((re.escape(name) for name in names), key=len, reverse=True)
    )
    start_pattern = re.compile(
        rf"^(?P<leader>.*?)(?<![A-Za-z0-9_])"
        rf"(?P<name>{alternatives})\((?P<body>.*)$"
    )
    resumed_pattern = re.compile(
        rf"^(?P<leader>.*?)<\.\.\. "
        rf"(?P<name>{alternatives}) resumed>(?P<body>.*)$"
    )
    unfinished_suffix = "<unfinished ...>"
    pending: dict[tuple[str, str], str] = {}
    result: list[str] = []

    for line in trace.splitlines():
        resumed = resumed_pattern.match(line)
        if resumed is not None:
            key = (resumed["leader"].rstrip(), resumed["name"])
            prefix = pending.pop(key, None)
            if prefix is None:
                prefix = (
                    f"{resumed['leader']}{resumed['name']}(<resumed without start> "
                )
            result.append(prefix + resumed["body"])
            continue

        started = start_pattern.match(line)
        if started is None:
            continue
        if line.rstrip().endswith(unfinished_suffix):
            key = (started["leader"].rstrip(), started["name"])
            previous = pending.get(key)
            if previous is not None:
                result.append(previous + "<missing resumed record>")
            marker_offset = line.rfind(unfinished_suffix)
            pending[key] = line[:marker_offset].rstrip()
            continue
        result.append(line)

    result.extend(
        prefix + "<missing resumed record>" for prefix in pending.values()
    )
    return result
