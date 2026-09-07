#!/usr/bin/env python3
"""Prove two live embedded PostgreSQL instances remain isolated for multi-instance runtime."""

from __future__ import annotations

import argparse
import json
import os
import re
import tempfile
from pathlib import Path
from typing import Any

from check_embedded_lifecycle import (
    LifecycleCheckError,
    input_identity,
    run_checked,
    runtime_environment,
    write_json,
)
from check_embedded_public_api import PublicApiCheckError, audit_trace


MARKER = "POSTGAMMA_RELEASE_MULTI_INSTANCE"
EVIDENCE_KIND = "postgamma.multi-instance"
RUNTIME_MARKER = "POSTGAMMA_RUNTIME"
REQUIRED_MARKER_VALUES = {
    "clusters": "2",
    "instances_opened": "3",
    "instances_live_peak": "2",
    "connections_opened": "9",
    "concurrent_connections": "8",
    "distinct_system_identifiers": "true",
    "data_isolation": "true",
    "guc_isolation": "true",
    "temp_isolation": "true",
    "notice_routing": "true",
    "notification_routing": "true",
    "simultaneous_active": "true",
    "parallel_query": "true",
    "cancel_peer_in_flight": "true",
    "cancel_isolation": "true",
    "checkpoints": "2",
    "first_closed": "true",
    "first_reopened": "true",
    "virtual_pids_valid": "true",
    "extension_sdk": "true",
    "bundled_extensions": "2",
    "vector_extension": "true",
    "vector_hnsw": "true",
    "vector_ivfflat": "true",
    "vector_guc_isolation": "true",
    "vector_concurrency": "true",
    "vector_parallel_query": "true",
    "vector_persistence": "true",
    "extension_instances": "3",
    "extension_sessions": "9",
    "extension_session_resets": "2",
    "extension_session_mobility": "true",
    "extension_parallel_workers": "true",
    "extension_descriptor_rejections": "17",
    "extension_resources": "true",
    "extension_failure_recovered": "true",
    "unbundled_rejected": "true",
    "capability_advertised": "true",
    "phase": "closed",
}
REQUIRED_RUNTIME_VALUES = {
    "backend_model": "thread",
    "provider": "pooled",
    "threads_started": "true",
    "role_process_launches": "0",
    "forbidden_process_launch_attempts": "0",
    "unsupported_role_requests": "0",
    "role_threads_active": "0",
    "runnable_sessions": "0",
    "running_quantums": "0",
    "pinned_sessions": "0",
    "blocked_sessions": "0",
    "execution_tokens_active": "0",
    "execution_token_rejections": "0",
}
RUNTIME_NUMERIC_FIELDS = (
    "host_pid",
    "role_completions",
    "pooled_worker_threads",
    "parallel_threads_started",
    "carrier_migrations",
    "execution_tokens_peak",
    "execution_token_budget",
)


class MultiInstanceCheckError(RuntimeError):
    """The dual-live instance contract was violated."""


def validate_instance_state_facts(
    runtime: dict[str, Any], policy: dict[str, Any]
) -> dict[str, Any]:
    if runtime.get("kind") != "postgamma.backend-state-runtime":
        raise MultiInstanceCheckError("backend-state runtime facts have invalid kind")
    if policy.get("kind") != "postgamma.backend-state-ownership":
        raise MultiInstanceCheckError("backend-state policy has invalid kind")
    owner_slots = runtime.get("owner_slots")
    owner_bytes = runtime.get("owner_bytes")
    slots = runtime.get("slots")
    decisions = policy.get("decisions")
    if (
        not isinstance(owner_slots, dict)
        or not isinstance(owner_bytes, dict)
        or not isinstance(slots, list)
        or not isinstance(decisions, list)
    ):
        raise MultiInstanceCheckError("backend-state facts are incomplete")
    instance_slots = [
        slot for slot in slots
        if isinstance(slot, dict) and slot.get("owner") == "instance"
    ]
    slot_ids = [slot.get("id") for slot in instance_slots]
    if any(not isinstance(identifier, str) for identifier in slot_ids):
        raise MultiInstanceCheckError("instance state contains an invalid slot identity")
    if len(set(slot_ids)) != len(slot_ids):
        raise MultiInstanceCheckError("instance state contains duplicate slot identities")
    slot_count = owner_slots.get("instance")
    byte_count = owner_bytes.get("instance")
    if (
        not isinstance(slot_count, int)
        or not isinstance(byte_count, int)
        or slot_count <= 0
        or byte_count <= 0
        or slot_count != len(instance_slots)
    ):
        raise MultiInstanceCheckError("instance state size or slot count is invalid")
    for slot in instance_slots:
        offset = slot.get("offset")
        size = slot.get("size")
        if (
            not isinstance(offset, int)
            or not isinstance(size, int)
            or offset < 0
            or size <= 0
            or offset + size > byte_count
        ):
            raise MultiInstanceCheckError(
                f"instance slot {slot.get('id')} exceeds its generated store"
            )
    policy_instance_ids: list[str] = []
    required_instance_ids: set[str] = set()
    for decision in decisions:
        if not isinstance(decision, dict) or decision.get("owner") != "instance":
            continue
        identifier = decision.get("id")
        if not isinstance(identifier, str):
            raise MultiInstanceCheckError("instance policy contains an invalid identity")
        policy_instance_ids.append(identifier)
        if decision.get("availability", "required") != "conditional":
            required_instance_ids.add(identifier)
    if len(set(policy_instance_ids)) != len(policy_instance_ids):
        raise MultiInstanceCheckError("instance policy contains duplicate identities")
    runtime_ids = set(slot_ids)
    policy_ids = set(policy_instance_ids)
    if not runtime_ids.issubset(policy_ids):
        raise MultiInstanceCheckError(
            "generated instance state contains an unreviewed policy identity"
        )
    missing_required = sorted(required_instance_ids - runtime_ids)
    if missing_required:
        raise MultiInstanceCheckError(
            "generated instance state omits required policy identities: "
            + ", ".join(missing_required)
        )
    return {
        "bytes": byte_count,
        "slots": slot_count,
        "slot_ids": sorted(slot_ids),
        "reviewed_policy_decisions": len(policy_instance_ids),
    }


def fields(line: str) -> dict[str, str]:
    return dict(re.findall(r"([a-z_]+)=([^\s]+)", line))


def parse_marker(stdout: str) -> dict[str, str | int]:
    lines = [line for line in stdout.splitlines() if line.startswith(f"{MARKER} ")]
    if len(lines) != 1:
        raise MultiInstanceCheckError(
            f"expected one {MARKER} line, found {len(lines)}"
        )
    values = fields(lines[0])
    for name, expected in REQUIRED_MARKER_VALUES.items():
        if values.get(name) != expected:
            raise MultiInstanceCheckError(
                f"multi-instance marker {name} is not {expected}"
            )
    try:
        host_pid = int(values.get("host_pid", ""))
    except ValueError as exc:
        raise MultiInstanceCheckError(
            "multi-instance host process identity is invalid"
        ) from exc
    if host_pid <= 0:
        raise MultiInstanceCheckError(
            "multi-instance host process identity is invalid"
        )
    return {**values, "host_pid": host_pid}


def parse_runtimes(stderr: str, host_pid: int) -> list[dict[str, str | int]]:
    lines = [line for line in stderr.splitlines() if RUNTIME_MARKER in line]
    if len(lines) != 3:
        raise MultiInstanceCheckError(
            f"expected three {RUNTIME_MARKER} lines, found {len(lines)}"
        )
    runtimes: list[dict[str, str | int]] = []
    parallel_instances = 0
    migrated_instances = 0
    for index, line in enumerate(lines):
        values = fields(line)
        for name, expected in REQUIRED_RUNTIME_VALUES.items():
            if values.get(name) != expected:
                raise MultiInstanceCheckError(
                    f"runtime {index} field {name} is not {expected}"
                )
        try:
            numeric = {
                name: int(values.get(name, ""))
                for name in RUNTIME_NUMERIC_FIELDS
            }
        except ValueError as exc:
            raise MultiInstanceCheckError(
                f"runtime {index} contains an invalid counter"
            ) from exc
        if numeric["host_pid"] != host_pid:
            raise MultiInstanceCheckError(
                f"runtime {index} belongs to another host process"
            )
        if numeric["pooled_worker_threads"] != 4:
            raise MultiInstanceCheckError(
                f"runtime {index} did not use four pooled workers"
            )
        if (
            numeric["execution_token_budget"] != 4
            or numeric["execution_tokens_peak"] > 4
        ):
            raise MultiInstanceCheckError(
                f"runtime {index} violated its execution-token bound"
            )
        if numeric["role_completions"] < 8:
            raise MultiInstanceCheckError(
                f"runtime {index} did not complete an instance lifecycle"
            )
        if numeric["execution_tokens_peak"] <= 0:
            raise MultiInstanceCheckError(
                f"runtime {index} did not execute a client quantum"
            )
        if numeric["parallel_threads_started"] > 0:
            parallel_instances += 1
        if numeric["carrier_migrations"] > 0:
            migrated_instances += 1
        runtimes.append({**values, **numeric})
    expected_fatal = (
        'FATAL:  bundled extension "postgamma_sdk_probe" failed during '
        "instance startup"
    )
    fatal_lines = [
        line for line in stderr.splitlines() if "FATAL:" in line
    ]
    if len(fatal_lines) != 1 or expected_fatal not in fatal_lines[0]:
        raise MultiInstanceCheckError(
            "runtime evidence does not contain the one expected extension "
            "startup failure"
        )
    if parallel_instances < 2:
        raise MultiInstanceCheckError(
            "fewer than two live instances executed parallel workers"
        )
    if migrated_instances < 2:
        raise MultiInstanceCheckError(
            "fewer than two extension-bearing instances migrated sessions"
        )
    if "PANIC:" in stderr:
        raise MultiInstanceCheckError(
            "multi-instance run emitted a backend panic"
        )
    return runtimes


def parse_extension_lifecycle(stderr: str) -> dict[str, Any]:
    library = [
        fields(line) for line in stderr.splitlines()
        if line.startswith("POSTGAMMA_EXTENSION_LIBRARY ")
    ]
    instances = [
        fields(line) for line in stderr.splitlines()
        if line.startswith("POSTGAMMA_EXTENSION ")
    ]
    sessions = [
        fields(line) for line in stderr.splitlines()
        if line.startswith("POSTGAMMA_EXTENSION_SESSION ")
    ]
    extension_ids = ("pgvector", "postgamma_sdk_probe")
    library_by_id = {item.get("id"): item for item in library}
    instances_by_id = {
        identifier: [item for item in instances if item.get("id") == identifier]
        for identifier in extension_ids
    }
    sessions_by_id = {
        identifier: [item for item in sessions if item.get("id") == identifier]
        for identifier in extension_ids
    }
    if set(library_by_id) != set(extension_ids) or any(
        library_by_id[identifier].get("initializations") != "1"
        for identifier in extension_ids
    ):
        raise MultiInstanceCheckError(
            "bundled extension library lifecycle cardinality is incomplete"
        )
    for identifier in extension_ids:
        module_instances = instances_by_id[identifier]
        if (
            len(module_instances) != 3
            or any(
                item.get("phase") != "closed"
                or item.get("instance_requests") != "1"
                or item.get("instance_startups") != "1"
                or item.get("instance_shutdowns") != "1"
                for item in module_instances
            )
            or len({item.get("generation") for item in module_instances}) != 3
        ):
            raise MultiInstanceCheckError(
                f"bundled extension {identifier} instance lifecycle is incomplete"
            )
    probe_sessions = sessions_by_id["postgamma_sdk_probe"]
    initialized = sum(
        item.get("phase") == "initialized" for item in probe_sessions
    )
    destroyed = sum(item.get("phase") == "destroyed" for item in probe_sessions)
    if (
        sessions_by_id["pgvector"]
        or len(probe_sessions) != 18
        or initialized != 9
        or destroyed != 9
    ):
        raise MultiInstanceCheckError(
            "bundled extension lifecycle cardinality is incomplete"
        )
    return {
        "library_initializations": 2,
        "instance_requests": sum(
            int(item["instance_requests"]) for item in instances
        ),
        "instance_startups": sum(
            int(item["instance_startups"]) for item in instances
        ),
        "instance_shutdowns": sum(
            int(item["instance_shutdowns"]) for item in instances
        ),
        "session_initializations": initialized,
        "session_destroys": destroyed,
        "modules": {
            identifier: {
                "library_initializations": 1,
                "instance_requests": 3,
                "instance_startups": 3,
                "instance_shutdowns": 3,
                "session_initializations": (
                    initialized if identifier == "postgamma_sdk_probe" else 0
                ),
                "session_destroys": (
                    destroyed if identifier == "postgamma_sdk_probe" else 0
                ),
            }
            for identifier in extension_ids
        },
    }


def traced_command(strace: str, trace: Path, driver: Path, arguments: list[Path]) -> list[str]:
    return [
        strace,
        "--seccomp-bpf",
        "-f",
        "-qq",
        "-o",
        str(trace),
        "-e",
        "trace=process,signal,network,chdir,fchdir,umask,setitimer",
        str(driver),
        *(str(argument) for argument in arguments),
    ]


def write_text(path: Path, content: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content, encoding="utf-8")


def require_success(completed: Any) -> None:
    if completed.returncode != 0:
        raise MultiInstanceCheckError(
            f"multi-instance driver failed ({completed.returncode})\n"
            f"stdout:\n{completed.stdout}\nstderr:\n{completed.stderr}"
        )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--resource-root", required=True, type=Path)
    parser.add_argument("--library", required=True, type=Path)
    parser.add_argument("--driver", required=True, type=Path)
    parser.add_argument("--work-root", required=True, type=Path)
    parser.add_argument("--stdout", required=True, type=Path)
    parser.add_argument("--stderr", required=True, type=Path)
    parser.add_argument("--trace", required=True, type=Path)
    parser.add_argument("--strace", default="strace")
    parser.add_argument("--state-policy", required=True, type=Path)
    parser.add_argument("--state-runtime", required=True, type=Path)
    parser.add_argument("--input", action="append", default=[], type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()

    generated = (args.stdout, args.stderr, args.trace, args.output)
    for path in generated:
        path.resolve().parent.mkdir(parents=True, exist_ok=True)
        path.resolve().unlink(missing_ok=True)
    try:
        resource_root = args.resource_root.resolve(strict=True)
        library = args.library.resolve(strict=True)
        driver = args.driver.resolve(strict=True)
        state_policy = args.state_policy.resolve(strict=True)
        state_runtime = args.state_runtime.resolve(strict=True)
        inputs = list(
            dict.fromkeys(
                [
                    state_policy,
                    state_runtime,
                    *(path.resolve(strict=True) for path in args.input),
                ]
            )
        )
        instance_state = validate_instance_state_facts(
            json.loads(state_runtime.read_text(encoding="utf-8")),
            json.loads(state_policy.read_text(encoding="utf-8")),
        )
        work_root = args.work_root.resolve()
        work_root.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(
            prefix="release-multi-instance-", dir=work_root
        ) as temporary:
            temporary_path = Path(temporary)
            if not (resource_root / "bin" / "postgres").is_file():
                raise MultiInstanceCheckError(
                    "resource pack has no bin/postgres identity"
                )
            environment = runtime_environment(resource_root)
            library_paths = [str(library.parent)]
            if environment.get("LD_LIBRARY_PATH"):
                library_paths.append(environment["LD_LIBRARY_PATH"])
            environment["LD_LIBRARY_PATH"] = os.pathsep.join(library_paths)
            environment["LC_ALL"] = "C"
            host_cwd = temporary_path / "unrelated-host-cwd"
            host_cwd.mkdir()
            environment["PWD"] = str(host_cwd)
            completed = run_checked(
                traced_command(
                    args.strace,
                    args.trace.resolve(),
                    driver,
                    [
                        temporary_path / "cluster-a",
                        temporary_path / "cluster-b",
                        resource_root / "bin" / "postgres",
                        resource_root,
                    ],
                ),
                environment=environment,
                cwd=host_cwd,
                timeout=300.0,
                check=False,
            )
            require_success(completed)
            marker = parse_marker(completed.stdout)
            runtimes = parse_runtimes(completed.stderr, int(marker["host_pid"]))
            extension_lifecycle = parse_extension_lifecycle(completed.stderr)
            trace_text = args.trace.resolve().read_text(encoding="utf-8")
            host_safety = audit_trace(trace_text, driver)

        write_text(args.stdout.resolve(), completed.stdout)
        write_text(args.stderr.resolve(), completed.stderr)
        document = {
            "schema_version": 1,
            "kind": EVIDENCE_KIND,
            "status": "pass",
            "postgresql_major": 19,
            "library": str(library),
            "driver": str(driver),
            "resource_root": str(resource_root),
            "marker": marker,
            "runtimes": runtimes,
            "extension_lifecycle": extension_lifecycle,
            "instance_state": instance_state,
            "host_safety": host_safety,
            "cwd_independent_provider": True,
            "capability_advertised": True,
            "inputs": input_identity([*inputs, library, driver]),
        }
        write_json(args.output.resolve(), document)
    except (
        LifecycleCheckError,
        MultiInstanceCheckError,
        OSError,
        PublicApiCheckError,
        ValueError,
    ) as exc:
        parser.error(str(exc))
    print(
        "multi-instance evidence: pass "
        "(two live clusters, routed cancellation, close/reopen isolation)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
