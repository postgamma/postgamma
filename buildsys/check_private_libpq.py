#!/usr/bin/env python3
"""Run and record the embedded bootstrap private libpq memory-transport proof."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import subprocess
import sys
import tempfile
from collections import Counter
from pathlib import Path
from typing import Any

from check_bootstrap_evidence import LEAF_KIND, baseline_identity
from postgresql_embedded_adapter import load_adapter as load_embedded_adapter


SCHEMA_VERSION = 1
TRACE_KIND = "postgamma.private-libpq-syscalls"
RUN_KIND = "postgamma.private-libpq-run"
NETWORK_SYSCALLS = frozenset(
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
        "recvmmsg",
        "recvmsg",
        "sendmmsg",
        "sendmsg",
        "sendto",
        "setsockopt",
        "shutdown",
        "socket",
        "socketpair",
    }
)
PROCESS_CREATION_SYSCALLS = frozenset({"fork", "vfork", "execveat"})
HOST_SIGNAL_DELIVERY_SYSCALLS = frozenset(
    {
        "kill",
        "pidfd_send_signal",
        "rt_sigqueueinfo",
        "tgkill",
        "tkill",
    }
)
INTERNAL_SYMBOL_PREFIXES = ("PQ", "pq", "pg_")


class PrivateLibpqCheckError(RuntimeError):
    """Private libpq executable evidence is missing, stale, or invalid."""


def _unique_object(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    counts = Counter(key for key, _value in pairs)
    duplicates = sorted(key for key, count in counts.items() if count > 1)
    if duplicates:
        raise PrivateLibpqCheckError(
            "duplicate JSON key(s): " + ", ".join(duplicates)
        )
    return dict(pairs)


def load_json(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(
            path.read_text(encoding="utf-8"), object_pairs_hook=_unique_object
        )
    except (OSError, json.JSONDecodeError, PrivateLibpqCheckError) as exc:
        raise PrivateLibpqCheckError(f"cannot read {path}: {exc}") from exc
    if not isinstance(value, dict):
        raise PrivateLibpqCheckError(f"{path}: top-level value must be an object")
    return value


def exact_int(value: Any, label: str, minimum: int = 0) -> int:
    if not isinstance(value, int) or isinstance(value, bool) or value < minimum:
        raise PrivateLibpqCheckError(f"{label} must be an integer >= {minimum}")
    return value


def validate_driver_report(document: dict[str, Any], iterations: int) -> dict[str, Any]:
    fields = {
        "status",
        "warmup_status",
        "warmup_checks",
        "iterations",
        "completed_iterations",
        "checks",
        "total_checks",
        "queue_capacity",
        "startup_packets",
        "query_packets",
        "copy_packets",
        "copy_bytes_before_response",
        "copy_bytes_received",
        "simultaneous_queue_saturation",
        "secure_read_calls",
        "secure_write_calls",
        "socket_wait_calls",
        "notice_count",
        "cancel_dispatches",
        "network_connect_calls",
        "optional_security_calls",
        "backend_pid",
        "connection_generation",
        "request_generation",
        "active_transports_after_close",
        "endpoint_references_after_close",
        "allocated_bytes_after_close",
        "host_pid",
        "host_pid_unchanged",
        "resources_restored",
        "baseline_descriptors",
        "baseline_threads",
        "baseline_children",
        "baseline_mappings",
        "baseline_sysv_mappings",
    }
    if set(document) != fields:
        raise PrivateLibpqCheckError("private libpq driver report schema is invalid")
    boolean_fields = {"host_pid_unchanged", "resources_restored"}
    for field in fields - boolean_fields:
        exact_int(document[field], field)
    if (
        document["status"] != 1
        or document["warmup_status"] != 1
        or document["warmup_checks"] < 14
        or document["iterations"] != iterations
        or document["completed_iterations"] != iterations
        or document["checks"] < 14
        or document["total_checks"] < iterations * 14
        or document["queue_capacity"] != 64
        or document["startup_packets"] != 1
        or document["query_packets"] != 5
        or document["copy_packets"] != 1
        or document["copy_bytes_before_response"] != 128
        or document["copy_bytes_received"] != 4096
        or document["simultaneous_queue_saturation"] != 1
        or document["secure_read_calls"] == 0
        or document["secure_write_calls"] == 0
        or document["socket_wait_calls"] == 0
        or document["notice_count"] != 1
        or document["cancel_dispatches"] != 1
        or document["network_connect_calls"] != 0
        or document["optional_security_calls"] != 0
        or document["backend_pid"] != 4242
        or document["connection_generation"] != 702001
        or document["request_generation"] != 702002
        or document["active_transports_after_close"] != 0
        or document["endpoint_references_after_close"] != 0
        or document["allocated_bytes_after_close"] != 0
        or document["host_pid"] <= 0
        or document["host_pid_unchanged"] is not True
        or document["resources_restored"] is not True
        or document["baseline_threads"] != 1
        or document["baseline_children"] != 0
        or document["baseline_descriptors"] < 3
        or document["baseline_mappings"] < 1
        or document["baseline_sysv_mappings"] != 0
    ):
        raise PrivateLibpqCheckError("private libpq driver proof is incomplete")
    return document


def traced_syscalls(trace: str) -> list[str]:
    return re.findall(r"(?:^|\s)([a-zA-Z_][a-zA-Z0-9_]*)\(", trace, re.MULTILINE)


def audit_trace(trace: str, driver: Path, iterations: int) -> dict[str, Any]:
    calls = traced_syscalls(trace)
    counts = Counter(calls)
    network = sorted(NETWORK_SYSCALLS.intersection(counts))
    process = sorted(PROCESS_CREATION_SYSCALLS.intersection(counts))
    signals = sorted(HOST_SIGNAL_DELIVERY_SYSCALLS.intersection(counts))
    if network:
        raise PrivateLibpqCheckError(
            "private libpq invoked network syscall(s): " + ", ".join(network)
        )
    if process:
        raise PrivateLibpqCheckError(
            "private libpq created a process with: " + ", ".join(process)
        )
    if signals:
        raise PrivateLibpqCheckError(
            "private libpq delivered a host signal with: " + ", ".join(signals)
        )
    exec_lines = [line for line in trace.splitlines() if "execve(" in line]
    if len(exec_lines) != 1 or str(driver) not in exec_lines[0]:
        raise PrivateLibpqCheckError(
            "syscall trace must contain only the driver's initial execve"
        )
    clone_lines = [
        line
        for line in trace.splitlines()
        if re.search(r"(?:^|\s)clone3?\(", line)
    ]
    unsafe_clones = [line for line in clone_lines if "CLONE_THREAD" not in line]
    if unsafe_clones:
        raise PrivateLibpqCheckError("private libpq used a non-thread clone")
    if len(clone_lines) < iterations:
        raise PrivateLibpqCheckError(
            "syscall trace did not observe every protocol fixture thread"
        )
    return {
        "schema_version": SCHEMA_VERSION,
        "kind": TRACE_KIND,
        "network_syscalls": 0,
        "process_creation_syscalls": 0,
        "host_signal_delivery_syscalls": 0,
        "thread_clone_attempts": len(clone_lines),
        "initial_execve_calls": 1,
        "traced_syscall_count": len(calls),
    }


def nm_dynamic_symbols(nm: str, library: Path) -> set[str]:
    try:
        completed = subprocess.run(
            [nm, "-D", "--defined-only", str(library)],
            check=False,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
    except OSError as exc:
        raise PrivateLibpqCheckError(f"cannot execute {nm}: {exc}") from exc
    if completed.returncode != 0:
        raise PrivateLibpqCheckError(
            "cannot inspect private libpq dynamic symbols: "
            + (completed.stderr.strip() or completed.stdout.strip())
        )
    result: set[str] = set()
    for line in completed.stdout.splitlines():
        fields = line.split()
        if fields:
            result.add(fields[-1].split("@", 1)[0])
    return result


def validate_source_report(
    document: dict[str, Any], embedded_adapter: dict[str, Any]
) -> int:
    expected_hooks = embedded_adapter["libpq_memory_hooks"]
    expected_hook_files = {
        hook["id"]: hook["source_file"] for hook in expected_hooks
    }
    expected_files = set(expected_hook_files.values())
    if (
        document.get("schema_version") != 1
        or document.get("kind") != "postgamma.private-libpq-source"
        or document.get("adapter_id") != embedded_adapter["id"]
        or document.get("hook_count") != len(expected_hooks)
        or not isinstance(document.get("files"), list)
        or len(document["files"]) != len(expected_files)
    ):
        raise PrivateLibpqCheckError("private libpq source report is invalid")
    observed_hook_files = {
        hook.get("id"): file.get("source_file")
        for file in document["files"]
        if isinstance(file, dict) and isinstance(file.get("hooks"), list)
        for hook in file["hooks"]
        if isinstance(hook, dict)
    }
    if (
        observed_hook_files != expected_hook_files
        or len(observed_hook_files) != document["hook_count"]
    ):
        raise PrivateLibpqCheckError("private libpq provider seam set is incomplete")
    return len(expected_hooks)


def validate_link_report(document: dict[str, Any], private_object: Path) -> None:
    retained = document.get("retained_symbols")
    undefined = document.get("undefined_external_symbols")
    namespaced = document.get("namespaced_symbols")
    localized_count = exact_int(
        document.get("localized_symbol_count"),
        "localized_symbol_count",
        minimum=1,
    )
    defined_count = exact_int(
        document.get("defined_before_localization"),
        "defined_before_localization",
        minimum=1,
    )
    if (
        document.get("schema_version") != 1
        or document.get("kind") != "postgamma.private-libpq-link"
        or document.get("output") != str(private_object)
        or document.get("output_sha256") != sha256(private_object)
        or not isinstance(retained, list)
        or not retained
        or len(retained) != len(set(retained))
        or not all(
            isinstance(name, str)
            and name.startswith(("postgamma_private_libpq_", "postgamma_memory_"))
            for name in retained
        )
        or defined_count != localized_count + len(retained)
        or document.get("namespace_prefix")
        != "postgamma_private_libpq_symbol_"
        or not isinstance(namespaced, dict)
        or not namespaced
        or len(namespaced) != len(set(namespaced.values()))
        or not all(
            isinstance(source, str)
            and source
            and isinstance(target, str)
            and target == "postgamma_private_libpq_symbol_" + source
            and target in retained
            and source not in retained
            for source, target in namespaced.items()
        )
        or not isinstance(undefined, list)
        or any(
            isinstance(name, str) and name.startswith(INTERNAL_SYMBOL_PREFIXES)
            for name in undefined
        )
    ):
        raise PrivateLibpqCheckError("private libpq link receipt is invalid")


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def artifact(path: Path, build_root: Path) -> dict[str, str]:
    try:
        relative = path.resolve(strict=True).relative_to(build_root).as_posix()
    except (OSError, ValueError) as exc:
        raise PrivateLibpqCheckError(
            f"private libpq artifact is missing or outside {build_root}: {path}"
        ) from exc
    return {"path": relative, "sha256": sha256(path)}


def write_text(path: Path, content: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content, encoding="utf-8")


def write_json(path: Path, document: dict[str, Any]) -> None:
    content = json.dumps(document, indent=2, sort_keys=True) + "\n"
    path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(
        mode="w",
        encoding="utf-8",
        dir=path.parent,
        prefix=f".{path.name}.",
        delete=False,
    ) as handle:
        handle.write(content)
        temporary = Path(handle.name)
    try:
        os.replace(temporary, path)
    finally:
        temporary.unlink(missing_ok=True)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--library", required=True, type=Path)
    parser.add_argument("--driver", required=True, type=Path)
    parser.add_argument("--private-object", required=True, type=Path)
    parser.add_argument("--source-report", required=True, type=Path)
    parser.add_argument("--link-report", required=True, type=Path)
    parser.add_argument("--transport-evidence", required=True, type=Path)
    parser.add_argument("--extension-manifest", required=True, type=Path)
    parser.add_argument("--run-report", required=True, type=Path)
    parser.add_argument("--trace", required=True, type=Path)
    parser.add_argument("--trace-report", required=True, type=Path)
    parser.add_argument("--stderr", required=True, type=Path)
    parser.add_argument("--iterations", type=int, default=25)
    parser.add_argument("--strace", default="strace")
    parser.add_argument("--nm", default="nm")
    parser.add_argument("--profile", required=True, type=Path)
    parser.add_argument("--upstream-manifest", required=True, type=Path)
    parser.add_argument("--adapter", required=True, type=Path)
    parser.add_argument("--embedded-adapter", required=True, type=Path)
    parser.add_argument("--configure-state", required=True, type=Path)
    parser.add_argument("--config-log", required=True, type=Path)
    parser.add_argument("--build-profile", required=True)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()

    output = args.output.resolve()
    output.unlink(missing_ok=True)
    try:
        if args.iterations < 1 or args.iterations > 1000:
            raise PrivateLibpqCheckError("iterations must be between 1 and 1000")
        library = args.library.resolve(strict=True)
        driver = args.driver.resolve(strict=True)
        private_object = args.private_object.resolve(strict=True)
        source_report = args.source_report.resolve(strict=True)
        link_report = args.link_report.resolve(strict=True)
        transport_evidence = args.transport_evidence.resolve(strict=True)
        extension_manifest = args.extension_manifest.resolve(strict=True)
        run_report = args.run_report.resolve()
        trace = args.trace.resolve()
        trace_report_path = args.trace_report.resolve()
        stderr = args.stderr.resolve()
        for generated in (run_report, trace, trace_report_path, stderr):
            generated.unlink(missing_ok=True)
        baseline, _profile = baseline_identity(
            args.upstream_manifest.resolve(),
            args.adapter.resolve(),
            args.embedded_adapter.resolve(),
            args.profile.resolve(),
            args.configure_state.resolve(),
            args.config_log.resolve(),
            args.build_profile,
        )
        embedded_adapter = load_embedded_adapter(
            args.embedded_adapter.resolve()
        )
        provider_hook_count = validate_source_report(
            load_json(source_report), embedded_adapter
        )
        validate_link_report(load_json(link_report), private_object)
        transport = load_json(transport_evidence)
        if transport.get("status") != "pass" or transport.get("baseline") != baseline:
            raise PrivateLibpqCheckError("bounded transport evidence is stale")
        manifest = load_json(extension_manifest)
        expected_exports = manifest.get("public_symbols")
        if not isinstance(expected_exports, list) or not all(
            isinstance(name, str) for name in expected_exports
        ):
            raise PrivateLibpqCheckError("static extension export manifest is invalid")
        actual_exports = nm_dynamic_symbols(args.nm, library)
        if actual_exports != set(expected_exports):
            raise PrivateLibpqCheckError(
                "shared-library exports differ from the exact pgm_bootstrap allowlist"
            )
        if any(name.startswith(INTERNAL_SYMBOL_PREFIXES) for name in actual_exports):
            raise PrivateLibpqCheckError("private PostgreSQL symbols are exported")

        command = [
            args.strace,
            "-f",
            "-qq",
            "-o",
            str(trace),
            "-e",
            "trace=network,process,signal",
            str(driver),
            str(library),
            str(args.iterations),
        ]
        try:
            completed = subprocess.run(
                command,
                check=False,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                timeout=60,
            )
        except (OSError, subprocess.TimeoutExpired) as exc:
            raise PrivateLibpqCheckError(
                f"private libpq driver could not run: {exc}"
            ) from exc
        write_text(run_report, completed.stdout)
        write_text(stderr, completed.stderr)
        if completed.returncode != 0:
            raise PrivateLibpqCheckError(
                "private libpq driver failed: "
                + (completed.stderr.strip() or completed.stdout.strip())
            )
        driver_report = validate_driver_report(
            load_json(run_report), args.iterations
        )
        syscall_report = audit_trace(
            trace.read_text(encoding="utf-8"), driver, args.iterations
        )
        write_json(trace_report_path, syscall_report)
        build_root = output.parent.parent.resolve()
        artifact_paths = [
            library,
            driver,
            private_object,
            source_report,
            link_report,
            transport_evidence,
            run_report,
            trace,
            trace_report_path,
            stderr,
        ]
        evidence = {
            "schema_version": SCHEMA_VERSION,
            "kind": LEAF_KIND,
            "claim": "private_libpq_memory_transport",
            "status": "pass",
            "baseline": baseline,
            "command": [str(Path(__file__).resolve()), *sys.argv[1:]],
            "artifacts": [artifact(path, build_root) for path in artifact_paths],
            "limitations": [
                "The protocol peer is a deterministic PostgreSQL v3 fixture, not a real backend role.",
                "The temporary pgm_bootstrap probe API is evidence scaffolding and not the product C ABI.",
                "TLS, GSSAPI, and socket-based cancellation are intentionally outside the embedded transport capability.",
                "The mapping baseline is captured after one successful warmup lifecycle so libc lazy thread arenas are not classified as product-owned leaks.",
            ],
            "metrics": {
                "iterations": driver_report["iterations"],
                "warmup_checks": driver_report["warmup_checks"],
                "checks": driver_report["total_checks"],
                "provider_hooks": provider_hook_count,
                "localized_symbols": load_json(link_report)[
                    "localized_symbol_count"
                ],
                "namespaced_symbols": len(
                    load_json(link_report)["namespaced_symbols"]
                ),
                "queue_capacity": driver_report["queue_capacity"],
                "copy_bytes": driver_report["copy_bytes_received"],
                "copy_bytes_before_response": driver_report[
                    "copy_bytes_before_response"
                ],
                "simultaneous_queue_saturation": True,
                "notice_count": driver_report["notice_count"],
                "cancel_dispatches": driver_report["cancel_dispatches"],
                "network_syscalls": syscall_report["network_syscalls"],
                "process_creation_syscalls": syscall_report[
                    "process_creation_syscalls"
                ],
                "host_signal_delivery_syscalls": syscall_report[
                    "host_signal_delivery_syscalls"
                ],
                "dynamic_exports": len(actual_exports),
                "host_pid": driver_report["host_pid"],
                "host_pid_unchanged": bool(
                    driver_report["host_pid_unchanged"]
                ),
                "resources_restored": bool(
                    driver_report["resources_restored"]
                ),
                "baseline_descriptors": driver_report[
                    "baseline_descriptors"
                ],
                "baseline_threads": driver_report["baseline_threads"],
                "baseline_children": driver_report["baseline_children"],
                "baseline_mappings": driver_report["baseline_mappings"],
            },
        }
        write_json(output, evidence)
    except (PrivateLibpqCheckError, OSError, ValueError) as exc:
        parser.error(str(exc))
    print(
        "private libpq evidence: pass "
        f"({args.iterations} lifecycle(s), 0 network syscalls)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
