#!/usr/bin/env python3
"""Compare a declarative SQL corpus through libpq and the embedded public API."""

from __future__ import annotations

import argparse
import json
import os
import re
import tempfile
from collections import Counter
from pathlib import Path
from typing import Any

from check_embedded_lifecycle import (
    LifecycleCheckError,
    input_identity,
    installed_prefix,
    run_checked,
    runtime_environment,
    write_json,
)
from check_embedded_public_api import PublicApiCheckError, audit_trace
from check_ordered_results import OrderedResultsCheckError, parse_runtime
from generate_embedded_conformance_cases import (
    ConformanceCorpusError,
    load_corpus,
)


CASE_MARKER = "POSTGAMMA_EMBEDDED_CONFORMANCE_CASE"
REFERENCE_MARKER = "POSTGAMMA_EMBEDDED_CONFORMANCE_REFERENCE"
CANDIDATE_MARKER = "POSTGAMMA_EMBEDDED_CONFORMANCE_CANDIDATE"
REFERENCE_KIND = "postgamma.embedded-conformance-reference"
CANDIDATE_KIND = "postgamma.embedded-conformance-candidate"
EVIDENCE_KIND = "postgamma.embedded-conformance"


class EmbeddedConformanceError(RuntimeError):
    """The embedded SQL conformance proof is incomplete or inconsistent."""


def fields(line: str) -> dict[str, str]:
    return dict(re.findall(r"([a-z_]+)=([^\s]*)", line))


def expected_cases(corpus: dict[str, Any]) -> list[dict[str, str]]:
    return [
        {
            "ordinal": str(index),
            "name": case["name"],
            "category": case["category"],
            "session": case["session"],
            "kind": case["expected_kind"],
            "sqlstate": case.get("expected_sqlstate", "-"),
        }
        for index, case in enumerate(corpus["cases"])
    ]


def parse_cases(output: str, corpus: dict[str, Any]) -> list[str]:
    lines = [
        line.strip()
        for line in output.splitlines()
        if line.startswith(f"{CASE_MARKER} ")
    ]
    expected = expected_cases(corpus)
    if len(lines) != len(expected):
        raise EmbeddedConformanceError(
            f"expected {len(expected)} conformance cases, found {len(lines)}"
        )
    required_fields = {
        "ordinal",
        "name",
        "category",
        "session",
        "kind",
        "sqlstate",
        "rows",
        "columns",
        "digest",
    }
    for index, (line, wanted) in enumerate(zip(lines, expected, strict=True)):
        observed = fields(line)
        if set(observed) != required_fields:
            raise EmbeddedConformanceError(
                f"case {index} fields differ from the transcript contract"
            )
        for name, value in wanted.items():
            if observed.get(name) != value:
                raise EmbeddedConformanceError(
                    f"case {index} field {name} is {observed.get(name)!r}, "
                    f"expected {value!r}"
                )
        if (
            not observed["rows"].isdigit()
            or not observed["columns"].isdigit()
            or re.fullmatch(r"[0-9a-f]{16}", observed["digest"]) is None
        ):
            raise EmbeddedConformanceError(f"case {index} result summary is invalid")
    return lines


def parse_marker(output: str, marker: str, case_count: int) -> dict[str, str]:
    lines = [line for line in output.splitlines() if line.startswith(f"{marker} ")]
    if len(lines) != 1:
        raise EmbeddedConformanceError(
            f"expected one {marker} line, found {len(lines)}"
        )
    values = fields(lines[0])
    expected = {
        "postgres": "19",
        "cases": str(case_count),
        "connections": "2",
        "phase": "closed",
    }
    if values != expected:
        raise EmbeddedConformanceError(f"{marker} fields changed: {values}")
    return values


def load_report(path: Path, kind: str) -> dict[str, Any]:
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise EmbeddedConformanceError(f"cannot read evidence {path}: {error}") from error
    if (
        not isinstance(document, dict)
        or document.get("schema_version") != 1
        or document.get("kind") != kind
        or document.get("status") != "pass"
    ):
        raise EmbeddedConformanceError(f"invalid evidence document: {path}")
    return document


def write_text(path: Path, content: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content, encoding="utf-8")


def prepend_library_path(environment: dict[str, str], path: Path) -> None:
    values = [str(path)]
    if environment.get("LD_LIBRARY_PATH"):
        values.append(environment["LD_LIBRARY_PATH"])
    environment["LD_LIBRARY_PATH"] = os.pathsep.join(values)


def category_counts(corpus: dict[str, Any]) -> dict[str, int]:
    counts = Counter(case["category"] for case in corpus["cases"])
    return {name: counts[name] for name in corpus["required_categories"]}


def run_reference(args: argparse.Namespace) -> None:
    generated = (args.stdout, args.stderr, args.server_log, args.output)
    for path in generated:
        path.resolve().parent.mkdir(parents=True, exist_ok=True)
        path.resolve().unlink(missing_ok=True)
    corpus_path = args.corpus.resolve(strict=True)
    corpus = load_corpus(corpus_path)
    reference_build = args.postgres_build.resolve(strict=True)
    driver = args.driver.resolve(strict=True)
    inputs = [path.resolve(strict=True) for path in args.input]
    work_root = args.work_root.resolve()
    work_root.mkdir(parents=True, exist_ok=True)
    completed = None
    server_log_text = ""
    reference_pid = 0
    with (
        tempfile.TemporaryDirectory(prefix="pgm-conf-ref-", dir=work_root) as temp,
        tempfile.TemporaryDirectory(prefix="pgm-conf-socket-") as socket_temp,
    ):
        temporary = Path(temp)
        install_root = temporary / "install"
        run_checked(
            [
                args.make,
                "-C",
                str(reference_build),
                f"-j{args.jobs}",
                "install",
                f"DESTDIR={install_root}",
            ],
            timeout=300.0,
        )
        prefix = installed_prefix(install_root)
        environment = runtime_environment(prefix)
        environment.update({"LC_ALL": "C", "TZ": "UTC"})
        data_directory = temporary / "data"
        socket_directory = Path(socket_temp)
        log_path = temporary / "postgres.log"
        run_checked(
            [
                str(prefix / "bin" / "initdb"),
                "--pgdata",
                str(data_directory),
                "--username=postgamma",
                "--auth=trust",
                "--encoding=UTF8",
                "--locale=C",
                "--no-sync",
                "--no-instructions",
            ],
            environment=environment,
            timeout=60.0,
        )
        config = data_directory / "postgresql.conf"
        config.write_text(
            config.read_text(encoding="utf-8")
            + "\nlisten_addresses = ''\n"
            + f"unix_socket_directories = '{socket_directory}'\n"
            + "port = 65436\n"
            + "fsync = off\n",
            encoding="utf-8",
        )
        started = False
        try:
            try:
                run_checked(
                    [
                        str(prefix / "bin" / "pg_ctl"),
                        "-D",
                        str(data_directory),
                        "-l",
                        str(log_path),
                        "-w",
                        "start",
                    ],
                    environment=environment,
                    timeout=60.0,
                )
            except LifecycleCheckError as error:
                if log_path.is_file():
                    server_log_text = log_path.read_text(encoding="utf-8")
                    write_text(args.server_log.resolve(), server_log_text)
                raise EmbeddedConformanceError(
                    "reference server failed to start\n" + server_log_text
                ) from error
            started = True
            reference_pid = int(
                (data_directory / "postmaster.pid")
                .read_text(encoding="utf-8")
                .splitlines()[0]
            )
            completed = run_checked(
                [str(driver), str(socket_directory), "65436"],
                environment=environment,
                timeout=120.0,
                check=False,
            )
        finally:
            if started:
                run_checked(
                    [
                        str(prefix / "bin" / "pg_ctl"),
                        "-D",
                        str(data_directory),
                        "-m",
                        "fast",
                        "-w",
                        "stop",
                    ],
                    environment=environment,
                    timeout=60.0,
                    check=False,
                )
            if log_path.is_file():
                server_log_text = log_path.read_text(encoding="utf-8")
    if completed is None:
        raise EmbeddedConformanceError("reference driver did not run")
    write_text(args.stdout.resolve(), completed.stdout)
    write_text(args.stderr.resolve(), completed.stderr)
    write_text(args.server_log.resolve(), server_log_text)
    if completed.returncode != 0:
        raise EmbeddedConformanceError(
            f"reference driver failed ({completed.returncode})\n"
            f"stdout:\n{completed.stdout}\nstderr:\n{completed.stderr}"
        )
    cases = parse_cases(completed.stdout, corpus)
    marker = parse_marker(completed.stdout, REFERENCE_MARKER, len(corpus["cases"]))
    if reference_pid <= 0:
        raise EmbeddedConformanceError("reference postmaster identity is invalid")
    write_json(
        args.output.resolve(),
        {
            "schema_version": 1,
            "kind": REFERENCE_KIND,
            "status": "pass",
            "postgresql_major": 19,
            "reference_process_id": reference_pid,
            "case_count": len(cases),
            "category_counts": category_counts(corpus),
            "marker": marker,
            "semantic_cases": cases,
            "inputs": input_identity([corpus_path, *inputs]),
        },
    )


def run_candidate(args: argparse.Namespace) -> None:
    generated = (args.stdout, args.stderr, args.trace, args.output)
    for path in generated:
        path.resolve().parent.mkdir(parents=True, exist_ok=True)
        path.resolve().unlink(missing_ok=True)
    corpus_path = args.corpus.resolve(strict=True)
    corpus = load_corpus(corpus_path)
    reference_path = args.reference.resolve(strict=True)
    reference = load_report(reference_path, REFERENCE_KIND)
    postgres_build = args.postgres_build.resolve(strict=True)
    library = args.library.resolve(strict=True)
    driver = args.driver.resolve(strict=True)
    inputs = [path.resolve(strict=True) for path in args.input]
    work_root = args.work_root.resolve()
    work_root.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="embedded-conformance-candidate-", dir=work_root) as temp:
        temporary = Path(temp)
        install_root = temporary / "install"
        run_checked(
            [
                args.make,
                "-C",
                str(postgres_build),
                f"-j{args.jobs}",
                "install",
                f"DESTDIR={install_root}",
            ],
            timeout=300.0,
        )
        prefix = installed_prefix(install_root)
        environment = runtime_environment(prefix)
        prepend_library_path(environment, library.parent)
        environment.update({"LC_ALL": "C", "TZ": "UTC"})
        command = [
            args.strace,
            "-f",
            "-qq",
            "-o",
            str(args.trace.resolve()),
            "-e",
            "trace=process,signal,network,chdir,fchdir,umask,setitimer",
            str(driver),
            str(temporary / "data"),
            str(prefix / "bin" / "postgres"),
            str(prefix),
            "create",
        ]
        completed = run_checked(
            command, environment=environment, timeout=180.0, check=False
        )
    write_text(args.stdout.resolve(), completed.stdout)
    write_text(args.stderr.resolve(), completed.stderr)
    if completed.returncode != 0:
        raise EmbeddedConformanceError(
            f"candidate driver failed ({completed.returncode})\n"
            f"stdout:\n{completed.stdout}\nstderr:\n{completed.stderr}"
        )
    cases = parse_cases(completed.stdout, corpus)
    marker = parse_marker(completed.stdout, CANDIDATE_MARKER, len(corpus["cases"]))
    if cases != reference.get("semantic_cases"):
        raise EmbeddedConformanceError(
            "embedded SQL transcript differs from the socket-libpq reference"
        )
    runtime = parse_runtime(completed.stderr)
    host_safety = audit_trace(
        args.trace.resolve().read_text(encoding="utf-8"), driver
    )
    write_json(
        args.output.resolve(),
        {
            "schema_version": 1,
            "kind": CANDIDATE_KIND,
            "status": "pass",
            "postgresql_major": 19,
            "case_count": len(cases),
            "category_counts": category_counts(corpus),
            "semantic_match": True,
            "marker": marker,
            "semantic_cases": cases,
            "runtime": runtime,
            "host_safety": host_safety,
            "inputs": input_identity([corpus_path, reference_path, *inputs]),
        },
    )


def run_aggregate(args: argparse.Namespace) -> None:
    corpus_path = args.corpus.resolve(strict=True)
    corpus = load_corpus(corpus_path)
    reference_path = args.reference.resolve(strict=True)
    candidate_path = args.candidate.resolve(strict=True)
    reference = load_report(reference_path, REFERENCE_KIND)
    candidate = load_report(candidate_path, CANDIDATE_KIND)
    if reference.get("semantic_cases") != candidate.get("semantic_cases"):
        raise EmbeddedConformanceError("aggregate SQL transcripts do not match")
    expected_count = len(corpus["cases"])
    counts = category_counts(corpus)
    if (
        reference.get("case_count") != expected_count
        or candidate.get("case_count") != expected_count
        or reference.get("category_counts") != counts
        or candidate.get("category_counts") != counts
    ):
        raise EmbeddedConformanceError("aggregate corpus coverage changed")
    host_safety = candidate.get("host_safety")
    if not isinstance(host_safety, dict) or any(
        host_safety.get(name) != 0
        for name in (
            "process_creation_calls",
            "host_signal_delivery_calls",
            "network_endpoint_calls",
            "process_global_state_calls",
        )
    ):
        raise EmbeddedConformanceError("candidate host-safety evidence is invalid")
    write_json(
        args.output.resolve(),
        {
            "schema_version": 1,
            "kind": EVIDENCE_KIND,
            "status": "pass",
            "postgresql_major": 19,
            "case_count": expected_count,
            "connection_count": 2,
            "category_counts": counts,
            "socket_libpq_matches_embedded_public_api": True,
            "candidate_host_safety": host_safety,
            "inputs": input_identity([corpus_path, reference_path, candidate_path]),
        },
    )


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser()
    phases = result.add_subparsers(dest="phase", required=True)

    reference = phases.add_parser("reference")
    reference.add_argument("--make", default="make")
    reference.add_argument("--jobs", type=int, default=1)
    reference.add_argument("--postgres-build", required=True, type=Path)
    reference.add_argument("--driver", required=True, type=Path)
    reference.add_argument("--corpus", required=True, type=Path)
    reference.add_argument("--work-root", required=True, type=Path)
    reference.add_argument("--stdout", required=True, type=Path)
    reference.add_argument("--stderr", required=True, type=Path)
    reference.add_argument("--server-log", required=True, type=Path)
    reference.add_argument("--input", action="append", default=[], type=Path)
    reference.add_argument("--output", required=True, type=Path)

    candidate = phases.add_parser("candidate")
    candidate.add_argument("--make", default="make")
    candidate.add_argument("--jobs", type=int, default=1)
    candidate.add_argument("--postgres-build", required=True, type=Path)
    candidate.add_argument("--library", required=True, type=Path)
    candidate.add_argument("--driver", required=True, type=Path)
    candidate.add_argument("--corpus", required=True, type=Path)
    candidate.add_argument("--reference", required=True, type=Path)
    candidate.add_argument("--work-root", required=True, type=Path)
    candidate.add_argument("--stdout", required=True, type=Path)
    candidate.add_argument("--stderr", required=True, type=Path)
    candidate.add_argument("--trace", required=True, type=Path)
    candidate.add_argument("--strace", default="strace")
    candidate.add_argument("--input", action="append", default=[], type=Path)
    candidate.add_argument("--output", required=True, type=Path)

    aggregate = phases.add_parser("aggregate")
    aggregate.add_argument("--corpus", required=True, type=Path)
    aggregate.add_argument("--reference", required=True, type=Path)
    aggregate.add_argument("--candidate", required=True, type=Path)
    aggregate.add_argument("--output", required=True, type=Path)
    return result


def main() -> int:
    args = parser().parse_args()
    try:
        if args.phase == "reference":
            run_reference(args)
            print("embedded conformance reference: pass (socket libpq on PG19)")
        elif args.phase == "candidate":
            run_candidate(args)
            print("embedded conformance candidate: pass (public API matches PG19)")
        else:
            run_aggregate(args)
            print("embedded conformance evidence: pass")
    except (
        OSError,
        ValueError,
        LifecycleCheckError,
        PublicApiCheckError,
        OrderedResultsCheckError,
        ConformanceCorpusError,
        EmbeddedConformanceError,
    ) as error:
        parser().error(str(error))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
