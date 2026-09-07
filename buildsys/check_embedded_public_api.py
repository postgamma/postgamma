#!/usr/bin/env python3
"""Prove the stable public C ABI against a real in-process PG19 kernel."""

from __future__ import annotations

import argparse
import json
import re
import tempfile
from collections import Counter
from pathlib import Path

from check_embedded_lifecycle import (
    LifecycleCheckError,
    input_identity,
    installed_prefix,
    run_checked,
    runtime_environment,
    write_json,
)


MARKER = "POSTGAMMA_KERNEL_PUBLIC_API"
REQUIRED_MARKER_VALUES = {
    "abi": "65539",
    "capabilities": "112639",
    "postgres": "19",
    "typed": "true",
    "binary": "true",
    "error": "true",
    "settings": "true",
    "notice": "true",
    "notification": "true",
    "cancel": "true",
    "timeout": "true",
    "timeout_cleanup_bounded": "true",
    "caller_driven": "true",
    "waitable": "true",
    "request_threads": "0",
    "provider": "pooled",
    "executor_workers": "4",
    "session_state": "true",
    "guc_state": "true",
    "temp_state": "true",
    "prepared_state": "true",
    "transaction_state": "true",
    "portal_state": "true",
    "holdable_cursor_migration": "true",
    "advisory_pinning": "true",
    "shell_process_rejected": "true",
    "concurrent_connections": "4",
    "error_contract": "true",
    "ownership": "true",
    "creation_umask": "true",
    "close_retry": "true",
    "recovery": "true",
    "phase": "closed",
}
API_POLICY_KIND = "postgamma.c-public-api-contract"
ABI_BASELINE_KIND = "postgamma.core-abi-baseline"
PHASE_RANK = {
    "cluster-lifecycle": 1,
    "session-execution": 2,
    "c-api-foundation": 3,
    "ordered-results": 4,
    "chunked-results": 5,
    "copy-streaming": 6,
    "runtime-events": 7,
    "arrow-management": 8,
    "logical-management": 9,
    "embedded-release": 10,
    "post-v1": 11,
}
FORBIDDEN_HEADER_TOKENS = (
    "postgres.h",
    "libpq-fe.h",
    "PGconn",
    "PGresult",
    "MemoryContext",
    "sigjmp_buf",
)
FORBIDDEN_SIGNAL_CALLS = frozenset(
    {"kill", "pidfd_send_signal", "rt_sigqueueinfo", "tgkill", "tkill"}
)
FORBIDDEN_NETWORK_CALLS = frozenset(
    {
        "accept",
        "accept4",
        "bind",
        "connect",
        "getpeername",
        "getsockname",
        "getsockopt",
        "listen",
        "recvfrom",
        "recvmsg",
        "sendmsg",
        "sendto",
        "setsockopt",
        "shutdown",
        "socket",
        "socketpair",
    }
)
FORBIDDEN_HOST_STATE_CALLS = frozenset(
    {"chdir", "fchdir", "setitimer", "umask"}
)
CALL_PATTERN = re.compile(r"(?:^|\s)([a-zA-Z_][a-zA-Z0-9_]*)\(")


class PublicApiCheckError(RuntimeError):
    """The public C API did not meet its embedded product contract."""


def load_expected_public_symbols(
    policy_path: Path, baseline_path: Path
) -> frozenset[str]:
    try:
        policy = json.loads(policy_path.read_text(encoding="utf-8"))
        baseline = json.loads(baseline_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise PublicApiCheckError(f"cannot load public ABI policy: {exc}") from exc
    if (
        not isinstance(policy, dict)
        or policy.get("schema_version") != 1
        or policy.get("kind") != API_POLICY_KIND
    ):
        raise PublicApiCheckError("public API policy has an unsupported schema")
    if (
        not isinstance(baseline, dict)
        or baseline.get("schema_version") != 1
        or baseline.get("kind") != ABI_BASELINE_KIND
    ):
        raise PublicApiCheckError("core ABI baseline has an unsupported schema")
    abi = policy.get("abi")
    if not isinstance(abi, dict) or abi.get("state") != "frozen":
        raise PublicApiCheckError("public core ABI is not frozen")
    delivered_through = abi.get("delivered_through")
    if delivered_through not in PHASE_RANK:
        raise PublicApiCheckError("public API delivery phase is invalid")
    raw_functions = policy.get("functions")
    if not isinstance(raw_functions, list):
        raise PublicApiCheckError("public API function policy is invalid")
    expected: set[str] = set()
    for function in raw_functions:
        if not isinstance(function, dict):
            raise PublicApiCheckError("public API function policy is invalid")
        name = function.get("name")
        phase = function.get("phase")
        if not isinstance(name, str) or phase not in PHASE_RANK:
            raise PublicApiCheckError("public API function policy is invalid")
        if PHASE_RANK[phase] <= PHASE_RANK[delivered_through]:
            if name in expected:
                raise PublicApiCheckError(f"duplicate public function {name}")
            expected.add(name)
    raw_frozen = baseline.get("functions")
    if not isinstance(raw_frozen, list):
        raise PublicApiCheckError("core ABI function baseline is invalid")
    frozen = {
        entry.get("name")
        for entry in raw_frozen
        if isinstance(entry, dict) and isinstance(entry.get("name"), str)
    }
    if len(frozen) != len(raw_frozen) or not frozen <= expected:
        raise PublicApiCheckError("delivered symbols do not preserve the core ABI")
    return frozenset(expected)


def audit_frontend_source(source: Path | list[Path]) -> dict[str, object]:
    sources = [source] if isinstance(source, Path) else source
    content = "\n".join(path.read_text(encoding="utf-8") for path in sources)
    forbidden = {
        "postgamma_thread_create": "runtime thread creation",
        "pthread_create": "POSIX thread creation",
        "pgm-request": "legacy request-thread naming",
    }
    found = [description for token, description in forbidden.items() if token in content]
    if found:
        raise PublicApiCheckError(
            "public frontend is not caller-driven: " + ", ".join(found)
        )
    return {
        "caller_driven": True,
        "request_thread_creation_sites": 0,
        "waitable_api": True,
        "source_count": len(sources),
    }


def parse_marker(stdout: str) -> dict[str, str | int]:
    lines = [line for line in stdout.splitlines() if MARKER in line]
    if len(lines) != 1:
        raise PublicApiCheckError(
            f"expected one {MARKER} line, found {len(lines)}"
        )
    values = dict(re.findall(r"([a-z_]+)=([^\s]+)", lines[0]))
    for name, expected in REQUIRED_MARKER_VALUES.items():
        if values.get(name) != expected:
            raise PublicApiCheckError(
                f"public API marker {name} is not {expected}"
            )
    try:
        cancel_elapsed_ms = int(values.get("cancel_elapsed_ms", ""))
        timeout_elapsed_ms = int(values.get("timeout_elapsed_ms", ""))
    except ValueError as exc:
        raise PublicApiCheckError(
            "public API cancellation latency is not numeric"
        ) from exc
    if cancel_elapsed_ms < 0 or cancel_elapsed_ms > 2000:
        raise PublicApiCheckError(
            "public API cancellation did not complete within 2 seconds"
        )
    if timeout_elapsed_ms < 0 or timeout_elapsed_ms > 2000:
        raise PublicApiCheckError(
            "public API timeout cleanup did not complete within 2 seconds"
        )
    return {
        **values,
        "cancel_elapsed_ms": cancel_elapsed_ms,
        "timeout_elapsed_ms": timeout_elapsed_ms,
    }


def audit_runtime(stderr: str) -> dict[str, object]:
    lines = [
        line for line in stderr.splitlines() if "POSTGAMMA_RUNTIME" in line
    ]
    if len(lines) != 1:
        raise PublicApiCheckError(
            f"expected one POSTGAMMA_RUNTIME line, found {len(lines)}"
        )
    values = dict(re.findall(r"([a-z_]+)=([^\s]+)", lines[0]))
    required = {
        "backend_model": "thread",
        "threads_started": "true",
        "role_process_launches": "0",
        "forbidden_process_launch_attempts": "0",
        "unsupported_role_requests": "0",
        "role_threads_active": "0",
        "provider": "pooled",
    }
    for name, expected in required.items():
        if values.get(name) != expected:
            raise PublicApiCheckError(
                f"runtime marker {name} is not {expected}"
            )
    numeric_names = (
        "client_threads_started",
        "client_threads_peak",
        "role_completions",
        "pooled_worker_threads",
        "client_quantums",
        "quantum_yields",
        "carrier_migrations",
        "runnable_sessions",
        "running_quantums",
        "pinned_sessions",
        "blocked_sessions",
        "execution_tokens_active",
        "execution_tokens_peak",
        "execution_token_budget",
        "execution_token_rejections",
    )
    try:
        numbers = {name: int(values.get(name, "")) for name in numeric_names}
    except ValueError as exc:
        raise PublicApiCheckError("runtime concurrency values are invalid") from exc
    clients = numbers["client_threads_started"]
    peak = numbers["client_threads_peak"]
    completions = numbers["role_completions"]
    workers = numbers["pooled_worker_threads"]
    if clients != 4 or workers != 4 or peak != 4 or completions < 4:
        raise PublicApiCheckError(
            "runtime did not prove four bounded pooled workers"
        )
    if (
        numbers["client_quantums"] <= 4
        or numbers["quantum_yields"] <= 4
        or numbers["carrier_migrations"] == 0
    ):
        raise PublicApiCheckError("runtime did not prove resumable client quantums")
    for name in (
        "runnable_sessions",
        "running_quantums",
        "pinned_sessions",
        "blocked_sessions",
        "execution_tokens_active",
        "execution_token_rejections",
    ):
        if numbers[name] != 0:
            raise PublicApiCheckError(f"runtime marker {name} is not zero")
    if (
        numbers["execution_token_budget"] != 4
        or numbers["execution_tokens_peak"] > 4
        or numbers["execution_tokens_peak"] == 0
    ):
        raise PublicApiCheckError("runtime execution token budget was violated")
    if "FATAL:" in stderr or "PANIC:" in stderr:
        raise PublicApiCheckError("public API run emitted a fatal backend error")
    return {
        "thread_model": True,
        "client_threads_started": clients,
        "client_threads_peak": peak,
        "role_completions": completions,
        "role_threads_active_after_close": 0,
        "provider": "pooled",
        "pooled_worker_threads": workers,
        "client_quantums": numbers["client_quantums"],
        "quantum_yields": numbers["quantum_yields"],
        "carrier_migrations": numbers["carrier_migrations"],
        "execution_token_budget": numbers["execution_token_budget"],
        "execution_tokens_peak": numbers["execution_tokens_peak"],
    }


def audit_trace(trace: str, driver: Path) -> dict[str, int]:
    calls = CALL_PATTERN.findall(trace)
    counts = Counter(calls)
    forbidden_signals = sorted(FORBIDDEN_SIGNAL_CALLS.intersection(counts))
    forbidden_network = sorted(FORBIDDEN_NETWORK_CALLS.intersection(counts))
    forbidden_host_state = sorted(
        FORBIDDEN_HOST_STATE_CALLS.intersection(counts)
    )
    if forbidden_signals:
        raise PublicApiCheckError(
            "public API delivered a host signal through: "
            + ", ".join(forbidden_signals)
        )
    if forbidden_network:
        raise PublicApiCheckError(
            "public API used an operating-system network endpoint through: "
            + ", ".join(forbidden_network)
        )
    if forbidden_host_state:
        raise PublicApiCheckError(
            "public API changed process-global host state through: "
            + ", ".join(forbidden_host_state)
        )
    if counts["fork"] or counts["vfork"] or counts["execveat"]:
        raise PublicApiCheckError("public API created a child process")
    exec_lines = [line for line in trace.splitlines() if "execve(" in line]
    if len(exec_lines) != 1 or str(driver) not in exec_lines[0]:
        raise PublicApiCheckError(
            "trace must contain only the public driver's initial execve"
        )
    clone_lines = [
        line for line in trace.splitlines() if re.search(r"\bclone3?\(", line)
    ]
    if not clone_lines or any("CLONE_THREAD" not in line for line in clone_lines):
        raise PublicApiCheckError(
            "public API trace contains a non-thread clone or no role threads"
        )
    delivered = re.findall(r"--- (SIG[A-Z0-9]+)", trace)
    if delivered:
        raise PublicApiCheckError(
            "public API received real process signals: "
            + ", ".join(sorted(set(delivered)))
        )
    return {
        "thread_clone_calls": len(clone_lines),
        "process_creation_calls": 0,
        "host_signal_delivery_calls": 0,
        "network_endpoint_calls": 0,
        "process_global_state_calls": 0,
        "traced_syscall_count": len(calls),
    }


def audit_symbols(
    nm: str, library: Path, expected_symbols: frozenset[str]
) -> dict[str, object]:
    completed = run_checked(
        [nm, "-D", "--defined-only", str(library)], timeout=30.0
    )
    symbols: set[str] = set()
    version_nodes: set[str] = set()
    for line in completed.stdout.splitlines():
        fields = line.split()
        if len(fields) < 3:
            continue
        name = fields[-1]
        if name == "POSTGAMMA_1.0":
            version_nodes.add(name)
            continue
        symbols.add(name.split("@", maxsplit=1)[0])
    if symbols != expected_symbols:
        missing = sorted(expected_symbols - symbols)
        unexpected = sorted(symbols - expected_symbols)
        raise PublicApiCheckError(
            f"public symbol mismatch: missing={missing}, unexpected={unexpected}"
        )
    if version_nodes != {"POSTGAMMA_1.0"}:
        raise PublicApiCheckError("public ABI version node is missing")
    return {
        "version_node": "POSTGAMMA_1.0",
        "exported_symbol_count": len(symbols),
        "exported_symbols": sorted(symbols),
        "postgres_symbols_exported": 0,
        "libpq_symbols_exported": 0,
    }


def audit_header(
    header: Path | list[Path], expected_symbols: frozenset[str]
) -> dict[str, object]:
    headers = [header] if isinstance(header, Path) else header
    content = "\n".join(path.read_text(encoding="utf-8") for path in headers)
    forbidden = [token for token in FORBIDDEN_HEADER_TOKENS if token in content]
    if forbidden:
        raise PublicApiCheckError(
            "public header leaks private PostgreSQL concepts: "
            + ", ".join(forbidden)
        )
    declarations = set(
        re.findall(
            r"\bPGM_API\b[^;]*?\b(pgm_[a-zA-Z0-9_]+)\s*\(",
            content,
            re.DOTALL,
        )
    )
    missing = expected_symbols - declarations
    if missing:
        raise PublicApiCheckError(
            "public header is missing implemented declarations: "
            + ", ".join(sorted(missing))
        )
    return {
        "postgres_headers": 0,
        "postgres_types": 0,
        "header_count": len(headers),
        "declared_symbol_count": len(declarations),
        "implemented_declaration_count": len(expected_symbols),
        "candidate_declaration_count": len(declarations - expected_symbols),
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--make", default="make")
    parser.add_argument("--jobs", type=int, default=1)
    parser.add_argument("--postgres-build", required=True, type=Path)
    parser.add_argument("--library", required=True, type=Path)
    parser.add_argument("--header", action="append", required=True, type=Path)
    parser.add_argument("--api-policy", required=True, type=Path)
    parser.add_argument("--abi-baseline", required=True, type=Path)
    parser.add_argument("--source", action="append", required=True, type=Path)
    parser.add_argument("--driver", required=True, type=Path)
    parser.add_argument("--work-root", required=True, type=Path)
    parser.add_argument("--stdout", required=True, type=Path)
    parser.add_argument("--stderr", required=True, type=Path)
    parser.add_argument("--trace", required=True, type=Path)
    parser.add_argument("--input", action="append", default=[], type=Path)
    parser.add_argument("--strace", default="strace")
    parser.add_argument("--nm", default="nm")
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()

    generated = (args.stdout, args.stderr, args.trace, args.output)
    for path in generated:
        path.resolve().parent.mkdir(parents=True, exist_ok=True)
        path.resolve().unlink(missing_ok=True)
    try:
        postgres_build = args.postgres_build.resolve(strict=True)
        library = args.library.resolve(strict=True)
        headers = [header.resolve(strict=True) for header in args.header]
        api_policy = args.api_policy.resolve(strict=True)
        abi_baseline = args.abi_baseline.resolve(strict=True)
        expected_symbols = load_expected_public_symbols(api_policy, abi_baseline)
        sources = [source.resolve(strict=True) for source in args.source]
        driver = args.driver.resolve(strict=True)
        inputs = [path.resolve(strict=True) for path in args.input]
        work_root = args.work_root.resolve()
        work_root.mkdir(parents=True, exist_ok=True)
        symbol_evidence = audit_symbols(args.nm, library, expected_symbols)
        header_evidence = audit_header(headers, expected_symbols)
        frontend_evidence = audit_frontend_source(sources)
        with tempfile.TemporaryDirectory(
            prefix="public-api-", dir=work_root
        ) as temporary:
            temporary_path = Path(temporary)
            install_root = temporary_path / "install"
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
            environment["LC_ALL"] = "C"
            data_directory = temporary_path / "data"
            command = [
                args.strace,
                "-f",
                "-qq",
                "-o",
                str(args.trace.resolve()),
                "-e",
                "trace=process,signal,network,chdir,fchdir,umask,setitimer",
                str(driver),
                str(data_directory),
                str(prefix / "bin" / "postgres"),
                str(prefix),
                "create",
            ]
            completed = run_checked(
                command, environment=environment, timeout=90.0, check=False
            )
            args.stdout.resolve().write_text(completed.stdout, encoding="utf-8")
            args.stderr.resolve().write_text(completed.stderr, encoding="utf-8")
            if completed.returncode != 0:
                raise PublicApiCheckError(
                    f"public API driver failed ({completed.returncode})\n"
                    f"stdout:\n{completed.stdout}\nstderr:\n{completed.stderr}"
                )
            marker = parse_marker(completed.stdout)
            runtime = audit_runtime(completed.stderr)
            trace = args.trace.resolve().read_text(encoding="utf-8")
            syscall_evidence = audit_trace(trace, driver)
        document = {
            "schema_version": 1,
            "kind": "postgamma.public-c-api",
            "status": "pass",
            "library": str(library),
            "driver": str(driver),
            "marker": marker,
            "public_header": header_evidence,
            "frontend_execution": frontend_evidence,
            "dynamic_symbols": symbol_evidence,
            "runtime": runtime,
            "cluster_creation": {
                "in_process": True,
                "created_from_missing_path": True,
                "external_initdb_invocations": 0,
            },
            "host_safety": syscall_evidence,
            "inputs": input_identity(inputs),
        }
        write_json(args.output.resolve(), document)
    except (LifecycleCheckError, OSError, PublicApiCheckError) as exc:
        parser.error(str(exc))
    print(
        "embedded public C API evidence: pass "
        "(typed results, callbacks, bounded timeout/cancel, 4 concurrent connections)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
