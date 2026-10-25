#!/usr/bin/env python3
"""Check authentication and configuration reload on a supplied server build."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import pwd
import shlex
import socket
import subprocess
import tempfile
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--prefix", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    prefix, out = args.prefix.resolve(), args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    data = out / "data"
    env = dict(os.environ, LD_LIBRARY_PATH=str(prefix / "lib"), LC_ALL="C",
               PGCONNECT_TIMEOUT="5", PGSSLMODE="disable", PGGSSENCMODE="disable")
    report = dict(status="running", cases=[],
                  binary_sha256=hashlib.sha256((prefix / "bin/postgres").read_bytes()).hexdigest())

    def run(name, command):
        result = subprocess.run([str(x) for x in command], env=env,
                                capture_output=True, text=True, timeout=60)
        (out / (name + ".stdout")).write_text(result.stdout)
        (out / (name + ".stderr")).write_text(result.stderr)
        result.check_returncode()
        return result.stdout

    def config(name, text):
        target = data / name
        temporary = target.with_suffix(".new")
        temporary.write_text(text)
        temporary.chmod(0o600)
        temporary.replace(target)

    try:
        run("initdb", [prefix / "bin/initdb", "-D", data, "--locale=C", "-A", "trust"])
        with tempfile.TemporaryDirectory(prefix="pg-auth-") as unix:
            with socket.socket() as probe:
                probe.bind(("127.0.0.1", 0))
                port = probe.getsockname()[1]
            psql = [prefix / "bin/psql", "-XAtw", "-v", "ON_ERROR_STOP=1",
                    "-p", str(port), "-d", "postgres"]
            options = ["-p", str(port), "-c", "listen_addresses=127.0.0.1",
                       "-c", "unix_socket_directories=" + unix, "-c", "shared_buffers=32MB"]
            try:
                run("start", [prefix / "bin/pg_ctl", "-D", data, "-l", out / "server.log",
                              "-o", shlex.join(options), "-w", "start"])
                run("role", [*psql, "-h", unix, "-c",
                             "SET password_encryption='scram-sha-256'; "
                             "CREATE ROLE auth_probe LOGIN PASSWORD 'probe-only-password'"])

                def reload():
                    # Observe postmaster's reload, rather than sleeping for an
                    # assumed scheduler delay or accepting a stale rule set.
                    log = out / "server.log"
                    offset = log.stat().st_size
                    run("reload", [prefix / "bin/pg_ctl", "-D", data, "reload"])
                    deadline = time.monotonic() + 10
                    while "received SIGHUP" not in log.read_text()[offset:]:
                        if time.monotonic() >= deadline:
                            raise RuntimeError("configuration reload was not observed")
                        time.sleep(.01)

                def check(name, host, password, success, fragment=""):
                    command = [str(x) for x in [*psql, "-h", host, "-U", "auth_probe",
                                               "-c", "SELECT current_user, 1"]]
                    result = subprocess.run(command, env=dict(env, PGPASSWORD=password),
                                            capture_output=True, text=True, timeout=15)
                    row = dict(name=name, returncode=result.returncode,
                               stdout=result.stdout, stderr=result.stderr)
                    report["cases"].append(row)
                    if ((result.returncode == 0) != success or
                        (success and result.stdout.strip() != "auth_probe|1") or
                        (not success and fragment not in result.stderr)):
                        raise RuntimeError("authentication contract failed: " + name)

                tail = "local all all trust\nhost all all 127.0.0.1/32 trust\n"
                scram = "host postgres auth_probe 127.0.0.1/32 scram-sha-256\n" + tail
                config("pg_hba.conf", scram)
                reload()
                check("scram_accept", "127.0.0.1", "probe-only-password", True)
                check("scram_wrong_password", "127.0.0.1", "incorrect", False,
                      "password authentication failed")
                config("pg_hba.conf", "host postgres auth_probe 127.0.0.1/32 reject\n" + tail)
                reload()
                check("hba_reject", "127.0.0.1", "probe-only-password", False,
                      "pg_hba.conf rejects connection")
                config("pg_hba.conf", scram)
                reload()
                check("reload_restores_accept", "127.0.0.1", "probe-only-password", True)
                config("pg_ident.conf", "probe_map " + pwd.getpwuid(os.getuid()).pw_name + " auth_probe\n")
                config("pg_hba.conf", "local postgres auth_probe peer map=probe_map\n" + tail)
                reload()
                check("peer_map_accept", unix, "", True)
                config("pg_ident.conf", "probe_map nonexistent_local_user auth_probe\n")
                reload()
                check("peer_map_reject", unix, "", False, "Peer authentication failed")
                report["status"] = "passed"
            finally:
                if (data / "postmaster.pid").exists():
                    run("stop", [prefix / "bin/pg_ctl", "-D", data, "-m", "fast", "-w", "stop"])
    except BaseException as error:
        report.update(status="failed", error=repr(error))
        raise
    finally:
        (out / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(dict(status=report["status"], cases=len(report["cases"]))))


if __name__ == "__main__":
    main()
