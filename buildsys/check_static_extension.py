#!/usr/bin/env python3
"""Verify and record the embedded bootstrap hidden static-extension proof."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shlex
import subprocess
import sys
import tempfile
from collections import Counter
from pathlib import Path
from typing import Any

from check_bootstrap_evidence import LEAF_KIND, baseline_identity
from c_source_probe import CSourceProbeError, sanitize_c, verify_source_seam
from generate_static_module_facts import (
    facts_report,
    load_json as load_extension_manifest,
    validate_manifest,
)
from link_bootstrap_static_extension import RECEIPT_KIND, REQUIRED_FLAGS
from postgresql_embedded_adapter import load_adapter as load_embedded_adapter


SYMBOL_REPORT_KIND = "postgamma.bootstrap-exported-symbols"
TRACE_REPORT_KIND = "postgamma.bootstrap-static-extension-syscalls"
NETWORK_SYSCALLS = frozenset(
    {
        "accept",
        "accept4",
        "bind",
        "connect",
        "listen",
        "recvfrom",
        "recvmsg",
        "sendmsg",
        "sendto",
        "socket",
        "socketpair",
    }
)
PROCESS_CREATION_SYSCALLS = frozenset({"fork", "vfork", "execveat"})
HOST_SIGNAL_DELIVERY_SYSCALLS = frozenset(
    {"kill", "pidfd_send_signal", "rt_sigqueueinfo", "tgkill", "tkill"}
)


class StaticExtensionCheckError(RuntimeError):
    """The hidden static-extension proof is incomplete or stale."""


def _unique_object(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    counts = Counter(key for key, _value in pairs)
    duplicates = sorted(key for key, count in counts.items() if count > 1)
    if duplicates:
        raise StaticExtensionCheckError(
            "duplicate JSON key(s): " + ", ".join(duplicates)
        )
    return dict(pairs)


def load_json(path: Path) -> dict[str, Any]:
    try:
        document = json.loads(
            path.read_text(encoding="utf-8"), object_pairs_hook=_unique_object
        )
    except (OSError, json.JSONDecodeError, StaticExtensionCheckError) as exc:
        raise StaticExtensionCheckError(f"cannot read {path}: {exc}") from exc
    if not isinstance(document, dict):
        raise StaticExtensionCheckError(f"{path}: top-level value must be an object")
    return document


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(command: list[str]) -> str:
    try:
        completed = subprocess.run(
            command,
            check=False,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
    except OSError as exc:
        raise StaticExtensionCheckError(
            f"cannot execute {command[0]}: {exc}"
        ) from exc
    if completed.returncode != 0:
        diagnostic = completed.stderr.strip() or completed.stdout.strip()
        raise StaticExtensionCheckError(
            f"command failed with status {completed.returncode}: "
            f"{shlex.join(command)}: {diagnostic}"
        )
    return completed.stdout


def traced_syscalls(trace: str) -> list[str]:
    return re.findall(r"(?:^|\s)([a-zA-Z_][a-zA-Z0-9_]*)\(", trace, re.MULTILINE)


def audit_trace(trace: str, driver: Path) -> dict[str, Any]:
    calls = traced_syscalls(trace)
    counts = Counter(calls)
    network = sorted(NETWORK_SYSCALLS.intersection(counts))
    process = sorted(PROCESS_CREATION_SYSCALLS.intersection(counts))
    signals = sorted(HOST_SIGNAL_DELIVERY_SYSCALLS.intersection(counts))
    if network:
        raise StaticExtensionCheckError(
            "static extension proof invoked network syscall(s): "
            + ", ".join(network)
        )
    if process:
        raise StaticExtensionCheckError(
            "static extension proof created a process with: "
            + ", ".join(process)
        )
    if signals:
        raise StaticExtensionCheckError(
            "static extension proof delivered a host signal with: "
            + ", ".join(signals)
        )
    exec_lines = [line for line in trace.splitlines() if "execve(" in line]
    if len(exec_lines) != 1 or str(driver) not in exec_lines[0]:
        raise StaticExtensionCheckError(
            "syscall trace must contain only the static driver's initial execve"
        )
    clone_lines = [
        line for line in trace.splitlines() if re.search(r"(?:^|\s)clone3?\(", line)
    ]
    if clone_lines:
        raise StaticExtensionCheckError("static extension proof created a thread")
    return {
        "schema_version": 1,
        "kind": TRACE_REPORT_KIND,
        "network_syscalls": 0,
        "process_creation_syscalls": 0,
        "host_signal_delivery_syscalls": 0,
        "thread_clone_attempts": 0,
        "initial_execve_calls": 1,
        "traced_syscall_count": len(calls),
    }


def nm_symbols(nm: str, library: Path, dynamic: bool, defined: bool) -> set[str]:
    command = [nm]
    if dynamic:
        command.append("-D")
    command.append("--defined-only" if defined else "--undefined-only")
    command.extend(("--format=posix", str(library)))
    symbols: set[str] = set()
    for line in run(command).splitlines():
        fields = line.split()
        if fields:
            symbols.add(fields[0].split("@", 1)[0])
    return symbols


def verify_provider_seam(
    postgres_source: Path, embedded_adapter: dict[str, Any]
) -> dict[str, Any]:
    try:
        return verify_source_seam(
            postgres_source, embedded_adapter["module_provider_seam"]
        )
    except CSourceProbeError as exc:
        raise StaticExtensionCheckError(str(exc)) from exc


def count_direct_dynamic_loader_calls(paths: list[Path]) -> dict[str, int]:
    counts = {"dlclose": 0, "dlopen": 0, "dlsym": 0}
    for path in paths:
        try:
            source = sanitize_c(path.read_text(encoding="utf-8"))
        except OSError as exc:
            raise StaticExtensionCheckError(
                f"cannot audit static provider source {path}: {exc}"
            ) from exc
        for name in counts:
            counts[name] += len(re.findall(rf"\b{name}\s*\(", source))
    return counts


def verify_link_receipt(receipt: dict[str, Any], library: Path) -> None:
    allowed = {
        "schema_version",
        "kind",
        "command",
        "work_directory",
        "required_flags",
        "inputs",
        "output",
        "output_sha256",
    }
    if set(receipt) != allowed:
        raise StaticExtensionCheckError("static-extension link receipt schema is invalid")
    if receipt["schema_version"] != 1 or receipt["kind"] != RECEIPT_KIND:
        raise StaticExtensionCheckError("static-extension link receipt kind is invalid")
    if receipt["required_flags"] != list(REQUIRED_FLAGS):
        raise StaticExtensionCheckError("static-extension link omitted required flags")
    command = receipt["command"]
    if not isinstance(command, list) or not all(
        isinstance(argument, str) and argument for argument in command
    ):
        raise StaticExtensionCheckError("static-extension link command is invalid")
    if any(flag not in command for flag in REQUIRED_FLAGS):
        raise StaticExtensionCheckError(
            "static-extension link command omitted a required flag"
        )
    if receipt["output"] != str(library) or receipt["output_sha256"] != sha256(library):
        raise StaticExtensionCheckError("static-extension link output is stale")
    inputs = receipt["inputs"]
    if not isinstance(inputs, dict) or not inputs:
        raise StaticExtensionCheckError("static-extension link receipt has no inputs")
    for value, digest in inputs.items():
        path = Path(value)
        if not path.is_file() or not isinstance(digest, str) or sha256(path) != digest:
            raise StaticExtensionCheckError(f"static-extension link input is stale: {path}")


def needed_libraries(readelf: str, library: Path) -> list[str]:
    output = run([readelf, "--dynamic", str(library)])
    return sorted(set(re.findall(r"Shared library: \[([^]]+)\]", output)))


def write_json(path: Path, document: dict[str, Any]) -> None:
    content = json.dumps(document, indent=2, sort_keys=True) + "\n"
    path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(
        mode="w", encoding="utf-8", dir=path.parent, prefix=f".{path.name}.", delete=False
    ) as handle:
        handle.write(content)
        temporary = Path(handle.name)
    try:
        os.replace(temporary, path)
    finally:
        temporary.unlink(missing_ok=True)


def write_text(path: Path, content: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content, encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--library", required=True, type=Path)
    parser.add_argument("--driver", required=True, type=Path)
    parser.add_argument("--link-receipt", required=True, type=Path)
    parser.add_argument("--facts-report", required=True, type=Path)
    parser.add_argument("--kernel-report", required=True, type=Path)
    parser.add_argument("--extension-manifest", required=True, type=Path)
    parser.add_argument("--postgres-source", required=True, type=Path)
    parser.add_argument("--profile", required=True, type=Path)
    parser.add_argument("--upstream-manifest", required=True, type=Path)
    parser.add_argument("--adapter", required=True, type=Path)
    parser.add_argument("--embedded-adapter", required=True, type=Path)
    parser.add_argument("--configure-state", required=True, type=Path)
    parser.add_argument("--config-log", required=True, type=Path)
    parser.add_argument("--build-profile", required=True)
    parser.add_argument("--nm", default="nm")
    parser.add_argument("--readelf", default="readelf")
    parser.add_argument("--strace", default="strace")
    parser.add_argument("--trace", required=True, type=Path)
    parser.add_argument("--trace-report", required=True, type=Path)
    parser.add_argument("--stderr", required=True, type=Path)
    parser.add_argument("--symbols-report", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    output = args.output.resolve()
    symbols_output = args.symbols_report.resolve()
    output.unlink(missing_ok=True)
    symbols_output.unlink(missing_ok=True)
    trace_output = args.trace.resolve()
    trace_report_output = args.trace_report.resolve()
    stderr_output = args.stderr.resolve()
    for generated in (trace_output, trace_report_output, stderr_output):
        generated.unlink(missing_ok=True)
    try:
        library = args.library.resolve(strict=True)
        driver = args.driver.resolve(strict=True)
        manifest_path = args.extension_manifest.resolve()
        project_root = Path(__file__).resolve().parents[1]
        manifest = validate_manifest(
            load_extension_manifest(manifest_path), project_root
        )
        baseline, _profile = baseline_identity(
            args.upstream_manifest.resolve(),
            args.adapter.resolve(),
            args.embedded_adapter.resolve(),
            args.profile.resolve(),
            args.configure_state.resolve(),
            args.config_log.resolve(),
            args.build_profile,
        )
        expected_facts = facts_report(manifest, manifest_path, project_root)
        if load_json(args.facts_report.resolve()) != expected_facts:
            raise StaticExtensionCheckError("static-module facts report is stale")
        receipt = load_json(args.link_receipt.resolve())
        verify_link_receipt(receipt, library)
        configure = load_json(args.configure_state.resolve())
        environment = configure.get("environment")
        if not isinstance(environment, dict) or "-fPIC" not in shlex.split(
            str(environment.get("CFLAGS", ""))
        ):
            raise StaticExtensionCheckError("embedded PostgreSQL build is not PIC")
        embedded_adapter = load_embedded_adapter(args.embedded_adapter.resolve())
        seam = verify_provider_seam(args.postgres_source.resolve(), embedded_adapter)
        provider_loader_calls = count_direct_dynamic_loader_calls(
            [
                project_root / "embedded-c/src/static_module_registry.c",
                project_root / "embedded-c/src/bootstrap_probe.c",
            ]
        )
        if any(provider_loader_calls.values()):
            raise StaticExtensionCheckError(
                "static module provider contains a direct dynamic-loader call"
            )

        probe_command = [
            args.strace,
            "-f",
            "-qq",
            "-o",
            str(trace_output),
            "-e",
            "trace=network,process,signal",
            str(driver),
            str(library),
        ]
        completed = subprocess.run(
            probe_command,
            check=False,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        write_text(stderr_output, completed.stderr)
        if completed.returncode != 0:
            raise StaticExtensionCheckError(
                "static extension driver failed: "
                + (completed.stderr.strip() or completed.stdout.strip())
            )
        probe = json.loads(completed.stdout)
        expected_probe_fields = {
            "status",
            "checks",
            "dynamic_load_attempts",
            "init_calls",
            "repeat_status",
            "repeat_init_calls",
            "host_symbol_interposed",
            "host_pid",
            "host_pid_unchanged",
            "resources_restored",
            "baseline_descriptors",
            "baseline_threads",
            "baseline_children",
            "baseline_mappings",
            "baseline_sysv_mappings",
            "library_mappings_after_close",
        }
        if not isinstance(probe, dict) or set(probe) != expected_probe_fields:
            raise StaticExtensionCheckError("static-extension driver output is invalid")
        if (
            probe["status"] != 1
            or probe["repeat_status"] != 1
            or probe["dynamic_load_attempts"] != 0
            or probe["init_calls"] != 1
            or probe["repeat_init_calls"] != 1
            or probe["host_symbol_interposed"] is not False
            or not isinstance(probe["host_pid"], int)
            or isinstance(probe["host_pid"], bool)
            or probe["host_pid"] <= 0
            or probe["host_pid_unchanged"] is not True
            or probe["resources_restored"] is not True
            or probe["baseline_threads"] != 1
            or probe["baseline_children"] != 0
            or probe["baseline_descriptors"] < 3
            or probe["baseline_mappings"] < 1
            or probe["baseline_sysv_mappings"] != 0
            or probe["library_mappings_after_close"] != 0
            or not isinstance(probe["checks"], int)
            or probe["checks"] < 10
        ):
            raise StaticExtensionCheckError("static-extension runtime probe failed")
        trace_report = audit_trace(
            trace_output.read_text(encoding="utf-8"), driver
        )
        write_json(trace_report_output, trace_report)

        dynamic_defined = nm_symbols(args.nm, library, dynamic=True, defined=True)
        local_defined = nm_symbols(args.nm, library, dynamic=False, defined=True)
        dynamic_undefined = nm_symbols(args.nm, library, dynamic=True, defined=False)
        public = set(manifest["public_symbols"])
        forbidden = set(manifest["forbidden_dynamic_symbols"])
        hidden_kernel = set(manifest["required_hidden_kernel_symbols"])
        unregistered = set(manifest["unregistered_linker_symbols"])
        registered = {
            symbol["linker_name"]
            for module in manifest["modules"]
            for symbol in module["symbols"]
        }
        if dynamic_defined != public:
            raise StaticExtensionCheckError(
                "dynamic exports differ from the exact pgm_* allowlist"
            )
        if dynamic_defined & forbidden or dynamic_defined & registered:
            raise StaticExtensionCheckError("private PostgreSQL symbols are exported")
        if not hidden_kernel.issubset(local_defined):
            raise StaticExtensionCheckError("required hidden kernel symbols are absent")
        if not registered.issubset(local_defined):
            raise StaticExtensionCheckError("registered extension symbols are absent")
        if "main" in local_defined:
            raise StaticExtensionCheckError("standalone PostgreSQL main was retained")
        if local_defined & unregistered:
            raise StaticExtensionCheckError(
                "an unregistered archive member was retained by the linker"
            )
        kernel_report = load_json(args.kernel_report.resolve())
        kernel_identity = kernel_report.get("identity")
        if not isinstance(kernel_identity, dict) or any(
            kernel_identity.get(field) != baseline[field]
            for field in (
                "upstream_commit",
                "configure_state_sha256",
                "toolchain_config_log_sha256",
                "embedded_adapter_id",
                "embedded_adapter_sha256",
            )
        ):
            raise StaticExtensionCheckError("kernel link report identity is stale")
        if kernel_report.get("response_sha256") not in receipt["inputs"].values():
            raise StaticExtensionCheckError(
                "kernel response is not bound to the final shared link"
            )
        if (
            kernel_report.get("catalog_registration_roots", {}).get("status")
            != "matched"
            or not kernel_report.get("included")
        ):
            raise StaticExtensionCheckError("kernel link closure is incomplete")
        needed = needed_libraries(args.readelf, library)
        symbols_report = {
            "schema_version": 1,
            "kind": SYMBOL_REPORT_KIND,
            "library_sha256": sha256(library),
            "dynamic_defined": sorted(dynamic_defined),
            "dynamic_undefined": sorted(dynamic_undefined),
            "required_hidden_kernel_symbols": sorted(hidden_kernel),
            "registered_extension_symbols": sorted(registered),
            "absent_unregistered_symbols": sorted(unregistered),
            "standalone_main_absent": True,
            "needed_libraries": needed,
            "provider_seam": seam,
            "static_provider_dynamic_loader_calls": provider_loader_calls,
        }
        write_json(symbols_output, symbols_report)
        artifact_root = output.parent.parent
        artifact_paths = [
            library,
            args.link_receipt.resolve(),
            args.facts_report.resolve(),
            args.kernel_report.resolve(),
            symbols_output,
            trace_output,
            trace_report_output,
            stderr_output,
        ]
        artifacts: list[dict[str, str]] = []
        for path in artifact_paths:
            try:
                relative = path.relative_to(artifact_root).as_posix()
            except ValueError as exc:
                raise StaticExtensionCheckError(
                    f"evidence artifact is outside the bootstrap build root: {path}"
                ) from exc
            artifacts.append({"path": relative, "sha256": sha256(path)})
        leaf = {
            "schema_version": 1,
            "kind": LEAF_KIND,
            "claim": "hidden_static_extension",
            "status": "pass",
            "baseline": baseline,
            "command": [str(Path(__file__).resolve()), *sys.argv[1:]],
            "artifacts": artifacts,
            "limitations": [
                "The PostgreSQL dfmgr provider seam is audited but not connected in bootstrap.",
                "A test-only shim replaces progname and parse_dispatch_option after main.o exclusion.",
                "This proof covers the current ELF build and is not a product-ready library.",
            ],
            "metrics": {
                "kernel_inputs": len(kernel_report["included"]),
                "registered_modules": len(manifest["modules"]),
                "registered_symbols": len(registered),
                "dynamic_exports": len(dynamic_defined),
                "runtime_checks": probe["checks"],
                "dynamic_load_attempts": probe["dynamic_load_attempts"],
                "direct_dynamic_loader_calls": sum(provider_loader_calls.values()),
                "module_init_calls": probe["init_calls"],
                "host_symbol_interpositions": int(probe["host_symbol_interposed"]),
                "host_pid": probe["host_pid"],
                "host_pid_unchanged": probe["host_pid_unchanged"],
                "resources_restored": probe["resources_restored"],
                "baseline_descriptors": probe["baseline_descriptors"],
                "baseline_threads": probe["baseline_threads"],
                "baseline_children": probe["baseline_children"],
                "baseline_mappings": probe["baseline_mappings"],
                "network_syscalls": trace_report["network_syscalls"],
                "process_creation_syscalls": trace_report[
                    "process_creation_syscalls"
                ],
                "host_signal_delivery_syscalls": trace_report[
                    "host_signal_delivery_syscalls"
                ],
            },
        }
        write_json(output, leaf)
    except (
        OSError,
        ValueError,
        json.JSONDecodeError,
        StaticExtensionCheckError,
    ) as exc:
        parser.error(str(exc))
    print(
        "static extension evidence: pass "
        f"({len(dynamic_defined)} export(s), {len(registered)} registered symbol(s))"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
