#!/usr/bin/env python3
"""Exercise transaction, wait and error recovery on the same socket sessions.

This validates the server protocol, not the embedded C or Python API.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import socket
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--prefix", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--iterations", type=int, default=20)
    parser.add_argument("--cc", default=os.environ.get("CC", "cc"))
    args = parser.parse_args()
    prefix = args.prefix.resolve()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    data = out / "data"
    report = {"status": "running", "scope": "server protocol transaction lifecycle",
              "prefix": str(prefix), "iterations": args.iterations, "commands": [],
              "binary_sha256": hashlib.sha256((prefix / "bin/postgres").read_bytes()).hexdigest()}
    env = dict(os.environ, LD_LIBRARY_PATH=str(prefix / "lib"), LC_ALL="C")

    def run(name, command, timeout=180):
        command = [str(x) for x in command]
        report["commands"].append({"name": name, "arguments": command})
        with (out / (name + ".stdout")).open("w") as stdout, (out / (name + ".stderr")).open("w") as stderr:
            result = subprocess.run(command, stdout=stdout, stderr=stderr, env=env, timeout=timeout)
        if result.returncode:
            raise RuntimeError(f"{name} failed ({result.returncode}); see {out}")

    try:
        driver = out / "transaction-chain"
        run("compile", [*shlex.split(args.cc), "-std=c11", "-O2", "-DNDEBUG",
                        "-Wall", "-Wextra", "-Werror", "-I" + str(prefix / "include"),
                        Path(__file__).with_name("transaction_chain.c"), "-L" + str(prefix / "lib"),
                        "-lpq", "-o", driver])
        run("initdb", [prefix / "bin/initdb", "-D", data, "--locale=C", "--encoding=UTF8", "-A", "trust"])
        with tempfile.TemporaryDirectory(prefix="pg-chain-") as socket_dir:
            with socket.socket() as probe:
                probe.bind(("127.0.0.1", 0))
                port = probe.getsockname()[1]
            options = ["-p", str(port), "-c", "listen_addresses=127.0.0.1",
                       "-c", f"unix_socket_directories={socket_dir}", "-c", "shared_buffers=64MB",
                       "-c", "fsync=on", "-c", "synchronous_commit=on", "-c", "max_connections=32"]
            try:
                run("start", [prefix / "bin/pg_ctl", "-D", data, "-l", out / "server.log",
                              "-o", shlex.join(options), "-w", "start"])
                run("chain", [driver, f"host=127.0.0.1 port={port} dbname=postgres connect_timeout=10 sslmode=disable",
                              args.iterations])
                report["result"] = json.loads((out / "chain.stdout").read_text())
                report["status"] = "passed"
            finally:
                if (data / "postmaster.pid").exists():
                    run("stop", [prefix / "bin/pg_ctl", "-D", data, "-m", "fast", "-w", "stop"])
    except BaseException as error:
        report.update(status="failed", error=repr(error))
        raise
    finally:
        (out / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report["result"]))


if __name__ == "__main__":
    main()
