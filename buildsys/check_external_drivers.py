#!/usr/bin/env python3
# Copyright 2026 Shujie Zhang. SPDX-License-Identifier: Apache-2.0
"""Compare ordinary external PostgreSQL drivers on fork and thread servers.

Requires a pgJDBC JAR, a JDK, and a Python interpreter with psycopg[binary]
and pg8000 installed. Dependencies are supplied by the caller, never installed
by this checker. The public in-process C/Python API is a separate interface.
"""

from __future__ import annotations

import argparse
from contextlib import contextmanager
import hashlib
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile

from check_threaded_runtime import available_port, linux_process_evidence, server_environment

ROOT = Path(__file__).resolve().parents[1]
USER = "driver_test"
PASSWORD = "public-driver-test-password"


@contextmanager
def cluster_directory():
    work = Path(tempfile.mkdtemp(prefix="postgamma-drivers-"))
    try:
        yield work
    finally:
        if (work / "data/postmaster.pid").exists():
            print(f"retaining cluster with a postmaster PID file: {work}", file=sys.stderr)
        else:
            shutil.rmtree(work)


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(command: list[str], log: Path, environment: dict[str, str], timeout: int = 120) -> str:
    result = subprocess.run(command, env=environment, capture_output=True, text=True, timeout=timeout)
    log.with_suffix(".stdout").write_text(result.stdout)
    log.with_suffix(".stderr").write_text(result.stderr)
    if result.returncode:
        raise RuntimeError(f"command failed ({result.returncode}); see {log}.stderr")
    return result.stdout


def check_server(args, model: str, prefix: Path, output: Path, classes: Path) -> dict:
    environment = {key: value for key, value in server_environment(prefix).items()
                   if not key.startswith("PG")}
    environment.update(LC_ALL="C", PGSSLMODE="disable", PGGSSENCMODE="disable")
    with cluster_directory() as work:
        data = work / "data"
        socket = work / "socket"
        socket.mkdir()
        port = available_port()
        run([str(prefix / "bin/initdb"), "-D", str(data), "--auth-local=trust",
             "--auth-host=scram-sha-256", "--encoding=UTF8", "--no-locale", "--no-sync"],
            output / "initdb", environment)
        options = shlex.join(["-p", str(port), "-c", "listen_addresses=127.0.0.1",
                              "-c", f"unix_socket_directories={socket}",
                              "-c", "shared_buffers=64MB", "-c", "max_connections=32"])
        start_attempted = False
        try:
            start_attempted = True
            run([str(prefix / "bin/pg_ctl"), "-D", str(data), "-l", str(output / "postgres.log"),
                 "-o", options, "-w", "start"], output / "start", environment)
            pid = int((data / "postmaster.pid").read_text().splitlines()[0])
            threads, children = linux_process_evidence(pid)
            if threads is not None and ((model == "thread" and children)
                                        or (model == "fork" and not children)):
                raise RuntimeError(f"unexpected {model} process topology: {children}")
            run([str(prefix / "bin/psql"), "-XAt", "-v", "ON_ERROR_STOP=1",
                 "-h", str(socket), "-p", str(port), "-d", "postgres", "-c",
                 f"SET password_encryption='scram-sha-256'; CREATE ROLE {USER} LOGIN PASSWORD '{PASSWORD}';"],
                output / "setup", environment)
            url = f"jdbc:postgresql://127.0.0.1:{port}/postgres"
            jdbc = run([args.java, "-cp", os.pathsep.join((str(classes), str(args.jdbc_jar))),
                        "JdbcCompatibility", url, USER, PASSWORD], output / "jdbc", environment)
            checks = [line.removeprefix("JDBC_CHECK ") for line in jdbc.splitlines()
                      if line.startswith("JDBC_CHECK ")]
            if len(checks) != 12 or len(set(checks)) != 12 or "JDBC_PASS 12" not in jdbc.splitlines():
                raise RuntimeError("JDBC suite did not complete all twelve checks")
            version = next((line.removeprefix("JDBC_VERSION ") for line in jdbc.splitlines()
                            if line.startswith("JDBC_VERSION ")), None)
            if version is None:
                raise RuntimeError("JDBC driver did not report its version")
            python = json.loads(run([args.python, str(ROOT / "tests/drivers/python_compatibility.py"),
                                    "--port", str(port), "--user", USER, "--password", PASSWORD],
                                   output / "python", environment))
            if python.get("status") != "passed" or [row["driver"] for row in python["drivers"]] != [
                    "psycopg", "pg8000"]:
                raise RuntimeError("Python drivers did not complete")
            for row, count in zip(python["drivers"], (9, 8)):
                if len(row["checks"]) != count or len(set(row["checks"])) != count:
                    raise RuntimeError(f"incomplete {row['driver']} suite")
            client_environment = dict(environment, PGPASSWORD=PASSWORD)
            libpq = run([str(args.reference_prefix / "bin/psql"), "-XAt", "-v", "ON_ERROR_STOP=1",
                         "-h", "127.0.0.1", "-p", str(port), "-U", USER, "-d", "postgres",
                         "-c", "SELECT 42,length(U&'\\03bb');"], output / "libpq", client_environment)
            if libpq.strip() != "42|1":
                raise RuntimeError("ordinary libpq SCRAM/UTF8 result changed")
            threads, children = linux_process_evidence(pid)
            if model == "thread" and children:
                raise RuntimeError("external clients caused role process launches")
            return {"model": model, "postgres_sha256": digest(prefix / "bin/postgres"),
                    "jdbc": {"version": version, "checks": checks}, "python": python["drivers"],
                    "libpq": {"checks": ["psql_scram_utf8"]}, "native_threads": threads,
                    "child_processes": sorted(children), "status": "passed"}
        finally:
            if start_attempted and (data / "postmaster.pid").exists():
                run([str(prefix / "bin/pg_ctl"), "-D", str(data), "-m", "fast", "-w", "stop"],
                    output / "stop", environment)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--reference-prefix", type=Path, required=True)
    parser.add_argument("--thread-prefix", type=Path, required=True)
    parser.add_argument("--jdbc-jar", type=Path, required=True)
    parser.add_argument("--java", default="java")
    parser.add_argument("--javac", default="javac")
    parser.add_argument("--python", default=sys.executable)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    for name in ("reference_prefix", "thread_prefix", "jdbc_jar", "output"):
        setattr(args, name, getattr(args, name).resolve())
    if hasattr(os, "geteuid") and os.geteuid() == 0:
        parser.error("run as a non-root user, as required by PostgreSQL initdb")
    if not args.jdbc_jar.is_file():
        parser.error(f"JDBC JAR does not exist: {args.jdbc_jar}")
    args.output.mkdir(parents=True, exist_ok=False)
    report = {"status": "running", "servers": [], "jdbc_jar_sha256": digest(args.jdbc_jar),
              "transport": "loopback TCP", "authentication": "scram-sha-256", "tls": "not-tested",
              "sources": {str(path.relative_to(ROOT)): digest(path) for path in (
                  ROOT / "tests/drivers/JdbcCompatibility.java", ROOT / "tests/drivers/python_compatibility.py",
                  Path(__file__).resolve())}}
    try:
        classes = args.output / "classes"
        classes.mkdir()
        run([args.java, "-version"], args.output / "java-version", dict(os.environ))
        # New JDKs warn that the supported Java 8 target is old; keep source diagnostics strict.
        run([args.javac, "--release", "8", "-Xlint:all,-options", "-Werror", "-cp", str(args.jdbc_jar),
             "-d", str(classes), str(ROOT / "tests/drivers/JdbcCompatibility.java")],
            args.output / "compile", dict(os.environ))
        for model, prefix in (("fork", args.reference_prefix), ("thread", args.thread_prefix)):
            output = args.output / model
            output.mkdir()
            report["servers"].append(check_server(args, model, prefix, output, classes))
            print(f"external drivers: {model} passed (JDBC 12, Psycopg 9, pg8000 8, libpq 1)", flush=True)
        if report["servers"][0]["jdbc"] != report["servers"][1]["jdbc"] or (
                report["servers"][0]["python"] != report["servers"][1]["python"]):
            raise RuntimeError("fork/thread driver coverage or versions differ")
        report["status"] = "passed"
        return 0
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        report["status"] = "failed"
        report["error"] = str(error)
        print(f"external drivers: {error}", file=sys.stderr)
        return 1
    finally:
        (args.output / "report.json").write_text(json.dumps(report, indent=2) + "\n")


if __name__ == "__main__":
    raise SystemExit(main())
