"""Recognize this checkout's managed preview before starting another server."""
from __future__ import annotations

import argparse
import ipaddress
from pathlib import Path
import re
import subprocess
from urllib.parse import urlsplit


def running_preview(root: Path, address: str) -> str | None:
    try:
        result = subprocess.run([
            "systemctl", "show", "postgamma-docs-preview.service",
            "--property=ActiveState,MainPID,WorkingDirectory,ControlGroup",
        ], capture_output=True, text=True, timeout=3)
        values = dict(line.split("=", 1) for line in result.stdout.splitlines() if "=" in line)
        if (result.returncode or values.get("ActiveState") != "active"
                or values.get("WorkingDirectory") != str(root.resolve())):
            return None
        # The unit itself calls make docs-serve. Its own process must launch
        # MkDocs; only a separate invocation should reuse that running service.
        groups = [line.split(":", 2)[-1] for line in Path("/proc/self/cgroup").read_text().splitlines()]
        if values.get("ControlGroup") in groups:
            return None
        command = (Path("/proc") / values["MainPID"] / "cmdline").read_bytes().decode().split("\0")
        options = dict(arg.split("=", 1) for arg in command if "=" in arg)
        bound = urlsplit("//" + options.get("DOCS_DEV_ADDR", "127.0.0.1:8000"))
        requested = urlsplit("//" + address)
        if bound.port != requested.port or bound.hostname not in {requested.hostname, "0.0.0.0", "::"}:
            return None
        host = bound.hostname
        if host in {"0.0.0.0", "::"}:
            certificate = options.get("DOCS_TLS_CERT")
            if certificate:
                result = subprocess.run(["openssl", "x509", "-in", certificate,
                                         "-noout", "-ext", "subjectAltName"],
                                        capture_output=True, text=True, timeout=3)
                addresses = [ipaddress.ip_address(value) for value in
                             re.findall(r"IP Address:([^,\s]+)", result.stdout)]
                addresses.sort(key=lambda value: (not value.is_global, value.version))
                if addresses:
                    host = str(addresses[0])
        if ":" in host:
            host = "[" + host + "]"
        return f"https://{host}:{bound.port}/"
    except (OSError, ValueError, KeyError, subprocess.SubprocessError):
        return None


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--address", required=True)
    args = parser.parse_args()
    url = running_preview(args.root, args.address)
    if url is None:
        return 1
    print("Documentation preview is already running: " + url)
    print("Managed by postgamma-docs-preview.service; documentation edits reload automatically.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
