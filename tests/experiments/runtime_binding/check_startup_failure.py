#!/usr/bin/env python3
"""Check failed async startup against a server linked with startup_failure.c."""
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
    args = parser.parse_args()
    prefix, out = args.prefix.resolve(), args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    marker, data = out / "fail-stack", out / "data"
    env = dict(os.environ, LD_LIBRARY_PATH=str(prefix / "lib"), LC_ALL="C",
               POSTGAMMA_TEST_STACK_FAILURE=str(marker), PGCONNECT_TIMEOUT="5")
    report = dict(status="running", failures=[],
                  binary_sha256=hashlib.sha256((prefix / "bin/postgres").read_bytes()).hexdigest())

    def run(name, command, success=True):
        result = subprocess.run([str(x) for x in command], env=env,
                                capture_output=True, text=True, timeout=60)
        (out / (name + ".stdout")).write_text(result.stdout)
        (out / (name + ".stderr")).write_text(result.stderr)
        if (result.returncode == 0) != success:
            raise RuntimeError("unexpected command result: " + name)
        return result.stdout.strip()

    try:
        run("initdb", [prefix / "bin/initdb", "-D", data, "--locale=C", "-A", "trust"])
        with tempfile.TemporaryDirectory(prefix="pg-startup-") as unix:
            with socket.socket() as probe:
                probe.bind(("127.0.0.1", 0))
                port = probe.getsockname()[1]
            options = ["-p", str(port), "-c", "listen_addresses=127.0.0.1",
                       "-c", "unix_socket_directories=" + unix,
                       "-c", "shared_buffers=32MB", "-c", "max_connections=16"]
            psql = [prefix / "bin/psql", "-XAtw", "-v", "ON_ERROR_STOP=1",
                    "-h", "127.0.0.1", "-p", str(port), "-d", "postgres"]
            try:
                run("start", [prefix / "bin/pg_ctl", "-D", data, "-l", out / "server.log",
                              "-o", shlex.join(options), "-w", "start"])
                pid = (data / "postmaster.pid").read_text().splitlines()[0]
                started = run("started", [*psql, "-c", "SELECT pg_postmaster_start_time()"])
                # Exceed connection capacity repeatedly; failed carriers must
                # return their slots as well as close the accepted sockets.
                marker.touch()
                for number in range(40):
                    run("failure-" + str(number), [*psql, "-c", "SELECT 1"], False)
                    report["failures"].append(number)
                marker.unlink()
                result = run("recovered", [*psql, "-c",
                    "BEGIN; CREATE TABLE startup_recovery(i int); "
                    "INSERT INTO startup_recovery VALUES (42); COMMIT; "
                    "SELECT i FROM startup_recovery"])
                if not result.endswith("42"):
                    raise RuntimeError("transaction after failed startups did not commit")
                after = run("after", [*psql, "-c", "SELECT pg_postmaster_start_time()"])
                failures = (out / "server.log").read_text().count(
                    "could not initialize backend thread stack: error ")
                if (after != started or
                    (data / "postmaster.pid").read_text().splitlines()[0] != pid or
                    failures != len(report["failures"])):
                    raise RuntimeError("startup failures were not isolated to their connections")
                report.update(status="passed", diagnosed_failures=failures,
                              recovered_transaction=True, same_postmaster=True)
            finally:
                marker.unlink(missing_ok=True)
                if (data / "postmaster.pid").exists():
                    run("stop", [prefix / "bin/pg_ctl", "-D", data, "-m", "fast", "-w", "stop"])
    except BaseException as error:
        report.update(status="failed", error=repr(error))
        raise
    finally:
        (out / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(dict(status=report["status"], failures=len(report["failures"]))))


if __name__ == "__main__":
    main()
