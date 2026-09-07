#!/usr/bin/env python3
"""Build, run, and record the embedded PG19 shutdown/recovery matrix."""

from __future__ import annotations

import argparse
import errno
import hashlib
import json
import os
import re
import subprocess
import tempfile
from collections import Counter
from pathlib import Path
from typing import Mapping, Sequence


MARKER = "POSTGAMMA_KERNEL_LIFECYCLE"
FOOTPRINT_MARKER = "POSTGAMMA_KERNEL_FOOTPRINT"
SUPERVISOR_STACK_MARKER = "POSTGAMMA_KERNEL_SUPERVISOR_STACK"
FAULT_MATRIX_MARKER = "POSTGAMMA_KERNEL_FAULT_MATRIX"
KERNEL_TELEMETRY_MARKER = "POSTGAMMA_KERNEL_TELEMETRY"
BACKEND_STACK_MARKER = "POSTGAMMA_STACK"
RUNTIME_MARKER = "POSTGAMMA_RUNTIME"
REQUIRED_MARKER_VALUES = {
    "cycles": "4",
    "smart_shutdown": "true",
    "fast_shutdown": "true",
    "immediate_shutdown": "true",
    "recovery_reopen": "true",
    "phase": "closed",
    "state": "closed",
    "resources_restored": "true",
    "mappings_stable": "true",
    "cwd_restored": "true",
    "locale_restored": "true",
    "environment_restored": "true",
    "signals_restored": "true",
    "pid_file_absent": "true",
    "role_process_launches": "0",
}
REQUIRED_LOG_MESSAGES = (
    "received smart shutdown request",
    "received fast shutdown request",
    "received immediate shutdown request",
    "database system was interrupted",
    "automatic recovery in progress",
    "database system is ready to accept connections",
)
RECOVERY_REDO_MESSAGES = (
    "redo starts at",
    "redo is not required",
)
FOOTPRINT_FIELDS = (
    "cold_open_to_ready_ms",
    "warm_open_to_ready_ms",
    "immediate_open_to_ready_ms",
    "recovery_open_to_ready_ms",
    "maximum_open_to_ready_ms",
    "peak_rss_delta_kib",
    "peak_virtual_delta_kib",
    "peak_thread_delta",
    "peak_fd_delta",
)
BUDGET_FIELDS = frozenset(
    {
        "max_cold_open_to_ready_ms",
        "max_warm_open_to_ready_ms",
        "max_immediate_open_to_ready_ms",
        "max_recovery_open_to_ready_ms",
        "max_any_open_to_ready_ms",
        "max_peak_rss_delta_kib",
        "max_peak_virtual_delta_kib",
        "max_peak_thread_delta",
        "max_peak_fd_delta",
        "expected_dedicated_threads_per_cycle",
        "min_supervisor_stack_bytes",
        "max_supervisor_stack_bytes",
        "min_dedicated_stack_bytes",
        "max_dedicated_stack_bytes",
        "min_client_stack_bytes",
        "max_client_stack_bytes",
        "min_stack_guard_bytes",
        "max_dedicated_peak_reservation_bytes",
        "max_client_peak_reservation_bytes",
    }
)
BACKEND_STACK_CLASSES = ("dedicated", "client", "parallel")
BACKEND_STACK_FIELDS = (
    "observations",
    "observation_failures",
    "depth_validations",
    "depth_validation_failures",
    "accounting_failures",
    "active_reservation_bytes",
    "peak_reservation_bytes",
    "configured_stack_min",
    "configured_stack_max",
    "configured_guard_min",
    "native_stack_min",
    "native_guard_min",
    "usable_stack_min",
    "depth_limit_min",
    "configured_depth_max",
)
FAULT_CYCLE_COUNT = 10
FAULT_CALLBACK_COUNT = sum(range(1, FAULT_CYCLE_COUNT + 1))
REQUIRED_FAULT_VALUES = {
    "generation": "1010",
    "cycles": str(FAULT_CYCLE_COUNT),
    "fault_points": str(FAULT_CYCLE_COUNT),
    "callbacks": str(FAULT_CALLBACK_COUNT),
    "injections": str(FAULT_CYCLE_COUNT),
    "cleanup_failures": "0",
    "failure_records": str(FAULT_CYCLE_COUNT),
    "host_fail_stop_records": str(FAULT_CYCLE_COUNT),
    "process_restart_required": "0",
    "supervisor_threads_exited": str(FAULT_CYCLE_COUNT),
    "supervisor_threads_joined": str(FAULT_CYCLE_COUNT),
    "resources_restored": "true",
    "mappings_stable": "true",
    "fault_status": str(errno.ECANCELED),
}
REQUIRED_KERNEL_TELEMETRY_VALUES = {
    "generation": "1010",
    "instances_entered": "14",
    "instances_closed": "4",
    "instances_failed": "10",
    "cleanup_failures": "0",
    "active_instances": "0",
    "active_memory_contexts": "0",
}
FORBIDDEN_SIGNAL_CALLS = frozenset(
    {"kill", "pidfd_send_signal", "rt_sigqueueinfo", "tgkill", "tkill"}
)
FORBIDDEN_NETWORK_CALLS = frozenset(
    {"accept", "accept4", "bind", "connect", "listen", "socketpair"}
)
FORBIDDEN_HOST_STATE_CALLS = frozenset(
    {"chdir", "fchdir", "setitimer", "umask"}
)
CALL_PATTERN = re.compile(r"(?:^|\s)([a-zA-Z_][a-zA-Z0-9_]*)\(")


class LifecycleCheckError(RuntimeError):
    """The embedded lifecycle did not meet its host-safety contract."""


def reject_duplicate_keys(
    pairs: list[tuple[str, object]],
) -> dict[str, object]:
    document: dict[str, object] = {}
    for name, value in pairs:
        if name in document:
            raise LifecycleCheckError(f"duplicate JSON key: {name}")
        document[name] = value
    return document


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def write_json(path: Path, document: object) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    content = json.dumps(document, indent=2, sort_keys=True) + "\n"
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


def run_checked(
    arguments: Sequence[str],
    *,
    environment: Mapping[str, str] | None = None,
    cwd: Path | None = None,
    timeout: float,
    check: bool = True,
) -> subprocess.CompletedProcess[str]:
    try:
        completed = subprocess.run(
            list(arguments),
            cwd=cwd,
            env=environment,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=timeout,
            check=False,
        )
    except (OSError, subprocess.TimeoutExpired) as exc:
        raise LifecycleCheckError(
            f"cannot execute {' '.join(arguments)}: {exc}"
        ) from exc
    if check and completed.returncode != 0:
        raise LifecycleCheckError(
            f"command failed ({completed.returncode}): {' '.join(arguments)}\n"
            f"stdout:\n{completed.stdout}\nstderr:\n{completed.stderr}"
        )
    return completed


def installed_prefix(install_root: Path) -> Path:
    candidates = sorted(install_root.glob("**/bin/postgres"))
    if len(candidates) != 1:
        raise LifecycleCheckError(
            "expected one installed postgres binary, found "
            f"{len(candidates)}"
        )
    return candidates[0].parent.parent


def runtime_environment(prefix: Path) -> dict[str, str]:
    environment = os.environ.copy()
    library_paths = [prefix / "lib", prefix / "lib64"]
    paths = [str(path) for path in library_paths if path.is_dir()]
    if environment.get("LD_LIBRARY_PATH"):
        paths.append(environment["LD_LIBRARY_PATH"])
    if paths:
        environment["LD_LIBRARY_PATH"] = os.pathsep.join(paths)
    return environment


def parse_marker(output: str) -> dict[str, str]:
    lines = [line for line in output.splitlines() if MARKER in line]
    if len(lines) != 1:
        raise LifecycleCheckError(
            f"expected one {MARKER} line, found {len(lines)}"
        )
    values = dict(re.findall(r"([a-z_]+)=([^\s]+)", lines[0]))
    try:
        generation = int(values.get("generation", ""))
    except ValueError as exc:
        raise LifecycleCheckError("lifecycle generation is not an integer") from exc
    if generation != 4:
        raise LifecycleCheckError("lifecycle generation is not four")
    for name, expected in REQUIRED_MARKER_VALUES.items():
        if values.get(name) != expected:
            raise LifecycleCheckError(
                f"lifecycle marker {name} is not {expected}"
            )
    return values


def parse_footprint_marker(output: str) -> dict[str, int | str]:
    lines = [line for line in output.splitlines() if FOOTPRINT_MARKER in line]
    if len(lines) != 1:
        raise LifecycleCheckError(
            f"expected one {FOOTPRINT_MARKER} line, found {len(lines)}"
        )
    raw = dict(re.findall(r"([a-z_]+)=([^\s]+)", lines[0]))
    try:
        generation = int(raw.get("generation", ""))
    except ValueError as exc:
        raise LifecycleCheckError(
            "footprint generation is not an integer"
        ) from exc
    if generation != 4:
        raise LifecycleCheckError("footprint generation is not four")
    if raw.get("profile") != "embedded-default":
        raise LifecycleCheckError("footprint profile is not embedded-default")
    measurements: dict[str, int | str] = {
        "generation": generation,
        "profile": raw["profile"],
    }
    for name in FOOTPRINT_FIELDS:
        try:
            value = int(raw.get(name, ""))
        except ValueError as exc:
            raise LifecycleCheckError(
                f"footprint {name} is not an integer"
            ) from exc
        if value <= 0:
            raise LifecycleCheckError(f"footprint {name} is not positive")
        measurements[name] = value
    return measurements


def parse_supervisor_stack_marker(output: str) -> dict[str, int | str]:
    lines = [
        line for line in output.splitlines()
        if SUPERVISOR_STACK_MARKER in line
    ]
    if len(lines) != 1:
        raise LifecycleCheckError(
            f"expected one {SUPERVISOR_STACK_MARKER} line, found {len(lines)}"
        )
    raw = dict(re.findall(r"([a-z_]+)=([^\s]+)", lines[0]))
    if raw.get("class") != "supervisor" or raw.get("actual_bounds") != "true":
        raise LifecycleCheckError("supervisor stack bounds are not actual")
    numeric_names = (
        "generation",
        "cycles_validated",
        "configured_stack_bytes",
        "configured_guard_bytes",
        "native_stack_bytes",
        "native_guard_bytes",
        "usable_stack_bytes",
    )
    values: dict[str, int | str] = {"class": "supervisor"}
    for name in numeric_names:
        try:
            value = int(raw.get(name, ""))
        except ValueError as exc:
            raise LifecycleCheckError(
                f"supervisor stack {name} is not an integer"
            ) from exc
        if value <= 0:
            raise LifecycleCheckError(
                f"supervisor stack {name} is not positive"
            )
        values[name] = value
    if values["generation"] != 4 or values["cycles_validated"] != 4:
        raise LifecycleCheckError("supervisor stack did not cover four cycles")
    return values


def parse_fault_matrix_marker(output: str) -> dict[str, int | str]:
    lines = [
        line for line in output.splitlines() if FAULT_MATRIX_MARKER in line
    ]
    if len(lines) != 1:
        raise LifecycleCheckError(
            f"expected one {FAULT_MATRIX_MARKER} line, found {len(lines)}"
        )
    raw = dict(re.findall(r"([a-z_]+)=([^\s]+)", lines[0]))
    for name, expected in REQUIRED_FAULT_VALUES.items():
        if raw.get(name) != expected:
            raise LifecycleCheckError(
                f"fault matrix marker {name} is not {expected}"
            )
    numeric_names = {
        "generation",
        "cycles",
        "fault_points",
        "callbacks",
        "injections",
        "cleanup_failures",
        "failure_records",
        "host_fail_stop_records",
        "process_restart_required",
        "supervisor_threads_exited",
        "supervisor_threads_joined",
        "fault_status",
    }
    parsed: dict[str, int | str] = dict(raw)
    for name in numeric_names:
        try:
            parsed[name] = int(raw[name])
        except ValueError as exc:
            raise LifecycleCheckError(
                f"fault matrix {name} is not an integer"
            ) from exc
    return parsed


def parse_kernel_telemetry_marker(output: str) -> dict[str, int]:
    lines = [
        line for line in output.splitlines()
        if KERNEL_TELEMETRY_MARKER in line
    ]
    if len(lines) != 1:
        raise LifecycleCheckError(
            f"expected one {KERNEL_TELEMETRY_MARKER} line, found {len(lines)}"
        )
    raw = dict(re.findall(r"([a-z_]+)=([^\s]+)", lines[0]))
    parsed: dict[str, int] = {}
    for name, expected in REQUIRED_KERNEL_TELEMETRY_VALUES.items():
        if raw.get(name) != expected:
            raise LifecycleCheckError(
                f"kernel telemetry marker {name} is not {expected}"
            )
        try:
            parsed[name] = int(raw[name])
        except ValueError as exc:
            raise LifecycleCheckError(
                f"kernel telemetry {name} is not an integer"
            ) from exc
    return parsed


def parse_backend_stack_telemetry(
    error_output: str,
    expected_generations: Sequence[int],
) -> list[dict[str, int | str]]:
    records: dict[tuple[int, str], dict[str, int | str]] = {}
    for line in error_output.splitlines():
        if BACKEND_STACK_MARKER not in line:
            continue
        raw = dict(re.findall(r"([a-z_]+)=([^\s]+)", line))
        class_name = raw.get("class", "")
        if class_name not in BACKEND_STACK_CLASSES:
            raise LifecycleCheckError(
                f"unknown backend stack class: {class_name}"
            )
        try:
            generation = int(raw.get("generation", ""))
        except ValueError as exc:
            raise LifecycleCheckError(
                "backend stack generation is not an integer"
            ) from exc
        record: dict[str, int | str] = {
            "generation": generation,
            "class": class_name,
        }
        for name in BACKEND_STACK_FIELDS:
            try:
                value = int(raw.get(name, ""))
            except ValueError as exc:
                raise LifecycleCheckError(
                    f"backend stack {name} is not an integer"
                ) from exc
            if value < 0:
                raise LifecycleCheckError(f"backend stack {name} is negative")
            record[name] = value
        key = (generation, class_name)
        if key in records:
            raise LifecycleCheckError(
                f"duplicate backend stack record: {generation}/{class_name}"
            )
        records[key] = record
    expected_keys = {
        (generation, class_name)
        for generation in expected_generations
        for class_name in BACKEND_STACK_CLASSES
    }
    if set(records) != expected_keys:
        missing = sorted(expected_keys.difference(records))
        extra = sorted(set(records).difference(expected_keys))
        raise LifecycleCheckError(
            f"backend stack records do not match expected set: "
            f"missing={missing}, extra={extra}"
        )
    return [records[key] for key in sorted(records)]


def load_resource_budget(path: Path) -> dict[str, object]:
    try:
        document = json.loads(
            path.read_text(encoding="utf-8"),
            object_pairs_hook=reject_duplicate_keys,
        )
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise LifecycleCheckError(
            f"cannot load resource budget {path}: {exc}"
        ) from exc
    if not isinstance(document, dict):
        raise LifecycleCheckError("resource budget root is not an object")
    expected = {
        "schema_version",
        "baseline_id",
        "postgresql_major",
        "profile",
        "measurement_source",
        "budgets",
    }
    if set(document) != expected:
        raise LifecycleCheckError("resource budget fields do not match schema")
    if document["schema_version"] != 1:
        raise LifecycleCheckError("resource budget schema_version is not one")
    if document["postgresql_major"] != 19:
        raise LifecycleCheckError("resource budget PostgreSQL major is not 19")
    if document["profile"] != "embedded-default":
        raise LifecycleCheckError("resource budget profile is not embedded-default")
    if document["measurement_source"] != "linux-procfs":
        raise LifecycleCheckError("resource budget source is not linux-procfs")
    baseline_id = document["baseline_id"]
    if not isinstance(baseline_id, str) or not baseline_id:
        raise LifecycleCheckError("resource budget baseline_id is empty")
    budgets = document["budgets"]
    if not isinstance(budgets, dict) or set(budgets) != BUDGET_FIELDS:
        raise LifecycleCheckError("resource budget limits do not match schema")
    for name, value in budgets.items():
        if type(value) is not int or value <= 0:
            raise LifecycleCheckError(
                f"resource budget {name} is not a positive integer"
            )
    return document


def validate_resource_budget(
    document: Mapping[str, object],
    measurements: Mapping[str, int | str],
) -> dict[str, object]:
    budgets = document["budgets"]
    if not isinstance(budgets, Mapping):
        raise LifecycleCheckError("resource budget limits are unavailable")
    limit_map = {
        "cold_open_to_ready_ms": "max_cold_open_to_ready_ms",
        "warm_open_to_ready_ms": "max_warm_open_to_ready_ms",
        "immediate_open_to_ready_ms": "max_immediate_open_to_ready_ms",
        "recovery_open_to_ready_ms": "max_recovery_open_to_ready_ms",
        "maximum_open_to_ready_ms": "max_any_open_to_ready_ms",
        "peak_rss_delta_kib": "max_peak_rss_delta_kib",
        "peak_virtual_delta_kib": "max_peak_virtual_delta_kib",
        "peak_thread_delta": "max_peak_thread_delta",
        "peak_fd_delta": "max_peak_fd_delta",
    }
    violations = []
    for measurement_name, budget_name in limit_map.items():
        measured = measurements[measurement_name]
        limit = budgets[budget_name]
        if not isinstance(measured, int) or not isinstance(limit, int):
            raise LifecycleCheckError("resource budget comparison is not numeric")
        if measured > limit:
            violations.append(
                f"{measurement_name}={measured} exceeds {budget_name}={limit}"
            )
    if violations:
        raise LifecycleCheckError(
            "embedded resource budget exceeded: " + "; ".join(violations)
        )
    return {
        "status": "pass",
        "baseline_id": document["baseline_id"],
        "postgresql_major": document["postgresql_major"],
        "profile": document["profile"],
        "measurement_source": document["measurement_source"],
        "measurements": dict(measurements),
        "budgets": dict(budgets),
    }


def validate_supervisor_stack_budget(
    document: Mapping[str, object],
    stack: Mapping[str, int | str],
) -> dict[str, object]:
    budgets = document["budgets"]
    if not isinstance(budgets, Mapping):
        raise LifecycleCheckError("supervisor stack budget is unavailable")
    configured = stack["configured_stack_bytes"]
    configured_guard = stack["configured_guard_bytes"]
    native = stack["native_stack_bytes"]
    native_guard = stack["native_guard_bytes"]
    usable = stack["usable_stack_bytes"]
    numeric = (configured, configured_guard, native, native_guard, usable)
    if not all(isinstance(value, int) for value in numeric):
        raise LifecycleCheckError("supervisor stack evidence is not numeric")
    if not (
        budgets["min_supervisor_stack_bytes"]
        <= configured
        <= budgets["max_supervisor_stack_bytes"]
    ):
        raise LifecycleCheckError("supervisor configured stack is outside budget")
    if configured_guard < budgets["min_stack_guard_bytes"]:
        raise LifecycleCheckError("supervisor configured guard is below budget")
    if native < configured or native_guard < configured_guard:
        raise LifecycleCheckError("supervisor native stack is below configuration")
    if usable != native - native_guard:
        raise LifecycleCheckError("supervisor usable stack is inconsistent")
    return {"status": "pass", "actual": dict(stack)}


def validate_backend_stack_budget(
    document: Mapping[str, object],
    records: Sequence[Mapping[str, int | str]],
    expected_counts: Mapping[str, int],
    expected_depth_validations: Mapping[str, int] | None = None,
) -> dict[str, object]:
    budgets = document["budgets"]
    if not isinstance(budgets, Mapping):
        raise LifecycleCheckError("backend stack budget is unavailable")
    if set(expected_counts) != set(BACKEND_STACK_CLASSES):
        raise LifecycleCheckError("backend stack expected classes are incomplete")
    if expected_depth_validations is None:
        expected_depth_validations = expected_counts
    if set(expected_depth_validations) != set(BACKEND_STACK_CLASSES):
        raise LifecycleCheckError(
            "backend stack depth-validation classes are incomplete"
        )
    class_limits = {
        "dedicated": (
            budgets["min_dedicated_stack_bytes"],
            budgets["max_dedicated_stack_bytes"],
            budgets["max_dedicated_peak_reservation_bytes"],
        ),
        "client": (
            budgets["min_client_stack_bytes"],
            budgets["max_client_stack_bytes"],
            budgets["max_client_peak_reservation_bytes"],
        ),
    }
    for record in records:
        class_name = record["class"]
        if not isinstance(class_name, str):
            raise LifecycleCheckError("backend stack class is not text")
        expected = expected_counts[class_name]
        expected_validations = expected_depth_validations[class_name]
        observations = record["observations"]
        observation_failures = record["observation_failures"]
        validations = record["depth_validations"]
        failures = record["depth_validation_failures"]
        if observations != expected:
            raise LifecycleCheckError(
                f"{class_name} stack observations={observations}, "
                f"expected={expected}"
            )
        if observation_failures != 0:
            raise LifecycleCheckError(
                f"{class_name} stack observation failed"
            )
        if validations != expected_validations or failures != 0:
            raise LifecycleCheckError(
                f"{class_name} stack-depth validation is incomplete"
            )
        if record["active_reservation_bytes"] != 0:
            raise LifecycleCheckError(
                f"{class_name} stack reservation remained active"
            )
        if record["accounting_failures"] != 0:
            raise LifecycleCheckError(
                f"{class_name} stack accounting failed"
            )
        if expected == 0:
            for name in BACKEND_STACK_FIELDS:
                if record[name] != 0:
                    raise LifecycleCheckError(
                        f"inactive {class_name} stack field {name} is nonzero"
                    )
            continue
        minimum, maximum, peak_limit = class_limits[class_name]
        configured_min = record["configured_stack_min"]
        configured_max = record["configured_stack_max"]
        configured_guard = record["configured_guard_min"]
        native = record["native_stack_min"]
        native_guard = record["native_guard_min"]
        usable = record["usable_stack_min"]
        depth_limit = record["depth_limit_min"]
        configured_depth = record["configured_depth_max"]
        peak = record["peak_reservation_bytes"]
        if not all(
            isinstance(value, int)
            for value in (
                minimum,
                maximum,
                peak_limit,
                configured_min,
                configured_max,
                configured_guard,
                native,
                native_guard,
                usable,
                depth_limit,
                configured_depth,
                peak,
            )
        ):
            raise LifecycleCheckError("backend stack evidence is not numeric")
        if not (
            minimum <= configured_min <= configured_max <= maximum
        ):
            raise LifecycleCheckError(
                f"{class_name} configured stack is outside budget"
            )
        if configured_guard < budgets["min_stack_guard_bytes"]:
            raise LifecycleCheckError(f"{class_name} guard is below budget")
        if native < configured_min or native_guard < configured_guard:
            raise LifecycleCheckError(
                f"{class_name} native stack is below configuration"
            )
        if usable != native - native_guard:
            raise LifecycleCheckError(
                f"{class_name} usable stack is inconsistent"
            )
        if configured_depth <= 0 or depth_limit < configured_depth:
            raise LifecycleCheckError(
                f"{class_name} max_stack_depth exceeds actual stack limit"
            )
        if peak < configured_max or peak > peak_limit:
            raise LifecycleCheckError(
                f"{class_name} peak stack reservation is outside budget"
            )
    return {
        "status": "pass",
        "records": [dict(record) for record in records],
    }


def validate_runtime_thread_contract(
    error_output: str,
    expected_cycles: int,
    expected_dedicated_threads: int,
) -> dict[str, object]:
    lines = [
        line for line in error_output.splitlines()
        if RUNTIME_MARKER in line
    ]
    if len(lines) != expected_cycles:
        raise LifecycleCheckError(
            f"expected {expected_cycles} {RUNTIME_MARKER} lines, "
            f"found {len(lines)}"
        )
    required = {
        "backend_model": "thread",
        "provider": "pooled",
        "threads_started": "true",
        "role_process_launches": "0",
        "forbidden_process_launch_attempts": "0",
        "unsupported_role_requests": "0",
    }
    numeric_names = (
        "host_pid",
        "client_threads_started",
        "parallel_threads_started",
        "dedicated_threads_started",
        "role_completions",
        "role_threads_active",
        "pooled_worker_threads",
        "execution_tokens_active",
        "execution_token_budget",
        "execution_token_rejections",
    )
    records: list[dict[str, int | str]] = []
    host_pid: int | None = None
    for cycle, line in enumerate(lines, start=1):
        raw = dict(re.findall(r"([a-z_]+)=([^\s]+)", line))
        for name, expected in required.items():
            if raw.get(name) != expected:
                raise LifecycleCheckError(
                    f"runtime cycle {cycle} marker {name} is not {expected}"
                )
        try:
            numeric = {
                name: int(raw.get(name, "")) for name in numeric_names
            }
        except ValueError as exc:
            raise LifecycleCheckError(
                f"runtime cycle {cycle} thread values are invalid"
            ) from exc
        if host_pid is None:
            host_pid = numeric["host_pid"]
        if numeric["host_pid"] <= 0 or numeric["host_pid"] != host_pid:
            raise LifecycleCheckError(
                "runtime lifecycle cycles did not share one host process"
            )
        expected = {
            "client_threads_started": 4,
            "parallel_threads_started": 0,
            "dedicated_threads_started": expected_dedicated_threads,
            "role_completions": expected_dedicated_threads,
            "role_threads_active": 0,
            "pooled_worker_threads": 4,
            "execution_tokens_active": 0,
            "execution_token_budget": 4,
            "execution_token_rejections": 0,
        }
        for name, expected_value in expected.items():
            if numeric[name] != expected_value:
                raise LifecycleCheckError(
                    f"runtime cycle {cycle} {name}={numeric[name]}, "
                    f"expected={expected_value}"
                )
        records.append({"cycle": cycle, **required, **numeric})
    return {
        "status": "pass",
        "cycles": expected_cycles,
        "host_pid": host_pid,
        "records": records,
    }


def validate_lifecycle_logs(error_output: str) -> dict[str, object]:
    missing = [
        message for message in REQUIRED_LOG_MESSAGES
        if message not in error_output
    ]
    if missing:
        raise LifecycleCheckError(
            "shutdown/reopen did not expose complete recovery evidence: "
            + ", ".join(missing)
        )
    redo_message = next(
        (message for message in RECOVERY_REDO_MESSAGES
         if message in error_output),
        None,
    )
    if redo_message is None:
        raise LifecycleCheckError(
            "shutdown/reopen did not expose a WAL redo conclusion: "
            + " or ".join(RECOVERY_REDO_MESSAGES)
        )
    return {
        "status": "pass",
        "messages": list(REQUIRED_LOG_MESSAGES),
        "redo_conclusion": redo_message,
    }


def audit_trace(trace: str, driver: Path) -> dict[str, object]:
    calls = CALL_PATTERN.findall(trace)
    counts = Counter(calls)
    forbidden_signals = sorted(FORBIDDEN_SIGNAL_CALLS.intersection(counts))
    forbidden_network = sorted(FORBIDDEN_NETWORK_CALLS.intersection(counts))
    forbidden_host_state = sorted(
        FORBIDDEN_HOST_STATE_CALLS.intersection(counts)
    )
    if forbidden_signals:
        raise LifecycleCheckError(
            "embedded lifecycle delivered a host signal through: "
            + ", ".join(forbidden_signals)
        )
    if forbidden_network:
        raise LifecycleCheckError(
            "embedded lifecycle used a network endpoint through: "
            + ", ".join(forbidden_network)
        )
    if forbidden_host_state:
        raise LifecycleCheckError(
            "embedded lifecycle changed process-global host state through: "
            + ", ".join(forbidden_host_state)
        )
    if counts["fork"] or counts["vfork"] or counts["execveat"]:
        raise LifecycleCheckError("embedded lifecycle created a child process")

    exec_lines = [line for line in trace.splitlines() if "execve(" in line]
    if len(exec_lines) != 1 or str(driver) not in exec_lines[0]:
        raise LifecycleCheckError(
            "trace must contain only the lifecycle driver's initial execve"
        )
    clone_lines = [
        line
        for line in trace.splitlines()
        if re.search(r"\bclone3?\(", line)
    ]
    non_thread_clones = [line for line in clone_lines if "CLONE_THREAD" not in line]
    if non_thread_clones:
        raise LifecycleCheckError(
            "embedded lifecycle issued clone without CLONE_THREAD"
        )
    if not clone_lines:
        raise LifecycleCheckError("embedded lifecycle did not create role threads")
    delivered_signals = re.findall(r"--- (SIG[A-Z0-9]+)", trace)
    if delivered_signals:
        raise LifecycleCheckError(
            "embedded lifecycle received real process signals: "
            + ", ".join(sorted(set(delivered_signals)))
        )
    return {
        "schema_version": 1,
        "kind": "postgamma.kernel-lifecycle-syscalls",
        "initial_execve_calls": 1,
        "thread_clone_calls": len(clone_lines),
        "process_creation_calls": 0,
        "host_signal_delivery_calls": 0,
        "network_endpoint_calls": 0,
        "process_global_cwd_calls": 0,
        "process_global_umask_calls": 0,
        "traced_syscall_count": len(calls),
    }


def input_identity(paths: Sequence[Path]) -> list[dict[str, str]]:
    return [
        {"path": str(path), "sha256": sha256(path)}
        for path in paths
    ]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--make", default="make")
    parser.add_argument("--jobs", type=int, default=1)
    parser.add_argument("--postgres-build", required=True, type=Path)
    parser.add_argument("--driver", required=True, type=Path)
    parser.add_argument("--work-root", required=True, type=Path)
    parser.add_argument("--stdout", required=True, type=Path)
    parser.add_argument("--stderr", required=True, type=Path)
    parser.add_argument("--run-report", required=True, type=Path)
    parser.add_argument("--trace", required=True, type=Path)
    parser.add_argument("--trace-report", required=True, type=Path)
    parser.add_argument("--budget", required=True, type=Path)
    parser.add_argument("--input", action="append", default=[], type=Path)
    parser.add_argument("--strace", default="strace")
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()

    generated_paths = (
        args.stdout,
        args.stderr,
        args.run_report,
        args.trace,
        args.trace_report,
        args.output,
    )
    for path in generated_paths:
        resolved = path.resolve()
        resolved.parent.mkdir(parents=True, exist_ok=True)
        resolved.unlink(missing_ok=True)
    try:
        postgres_build = args.postgres_build.resolve(strict=True)
        driver = args.driver.resolve(strict=True)
        work_root = args.work_root.resolve()
        work_root.mkdir(parents=True, exist_ok=True)
        inputs = [path.resolve(strict=True) for path in args.input]
        budget_document = load_resource_budget(args.budget.resolve(strict=True))
        with tempfile.TemporaryDirectory(
            prefix="lifecycle-", dir=work_root
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
            data_directory = temporary_path / "data"
            run_checked(
                [
                    str(prefix / "bin" / "initdb"),
                    "-D",
                    str(data_directory),
                    "--auth=trust",
                    "--encoding=UTF8",
                    "--locale=C",
                    "--no-sync",
                    "--no-instructions",
                ],
                environment=environment,
                timeout=60.0,
            )
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
            ]
            completed = run_checked(
                command, environment=environment, timeout=60.0, check=False
            )
            args.stdout.parent.mkdir(parents=True, exist_ok=True)
            args.stdout.write_text(completed.stdout, encoding="utf-8")
            args.stderr.write_text(completed.stderr, encoding="utf-8")
            if completed.returncode != 0:
                raise LifecycleCheckError(
                    f"command failed ({completed.returncode}): "
                    f"{' '.join(command)}\n"
                    f"stdout:\n{completed.stdout}\n"
                    f"stderr:\n{completed.stderr}"
                )
            marker = parse_marker(completed.stdout)
            footprint = parse_footprint_marker(completed.stdout)
            supervisor_stack = parse_supervisor_stack_marker(completed.stdout)
            fault_matrix = parse_fault_matrix_marker(completed.stdout)
            kernel_telemetry = parse_kernel_telemetry_marker(
                completed.stdout
            )
            resource_budget = validate_resource_budget(
                budget_document, footprint
            )
            supervisor_stack_budget = validate_supervisor_stack_budget(
                budget_document, supervisor_stack
            )
            expected_dedicated = budget_document["budgets"][
                "expected_dedicated_threads_per_cycle"
            ]
            if not isinstance(expected_dedicated, int):
                raise LifecycleCheckError(
                    "dedicated thread expectation is not numeric"
                )
            backend_stack = validate_backend_stack_budget(
                budget_document,
                parse_backend_stack_telemetry(
                    completed.stderr, tuple(range(1, 5))
                ),
                {
                    "dedicated": expected_dedicated,
                    "client": 0,
                    "parallel": 0,
                },
            )
            runtime_threads = validate_runtime_thread_contract(
                completed.stderr, 4, expected_dedicated
            )
            recovery = validate_lifecycle_logs(completed.stderr)
            if (data_directory / "postmaster.pid").exists():
                raise LifecycleCheckError("embedded lifecycle left postmaster.pid")

        trace_report = audit_trace(args.trace.read_text(encoding="utf-8"), driver)
        write_json(args.trace_report, trace_report)
        run_report = {
            "schema_version": 1,
            "kind": "postgamma.kernel-lifecycle-run",
            "status": "pass",
            "generation": 4,
            "cycles": 4,
            "marker": marker,
            "shutdown_and_recovery": recovery,
            "fault_matrix": fault_matrix,
            "kernel_telemetry": kernel_telemetry,
            "resource_budget": resource_budget,
            "stack_contract": {
                "supervisor": supervisor_stack_budget,
                "backends": backend_stack,
            },
            "thread_contract": runtime_threads,
        }
        write_json(args.run_report, run_report)
        report = {
            "schema_version": 1,
            "kind": "postgamma.kernel-lifecycle-evidence",
            "status": "pass",
            "claim": (
                "shutdown_recovery_and_deterministic_startup_fault_cleanup"
            ),
            "postgresql_major": 19,
            "driver_sha256": sha256(driver),
            "inputs": input_identity(inputs),
            "run": run_report,
            "syscalls": trace_report,
            "artifacts": {
                "stdout": str(args.stdout.resolve()),
                "stderr": str(args.stderr.resolve()),
                "run_report": str(args.run_report.resolve()),
                "trace": str(args.trace.resolve()),
                "trace_report": str(args.trace_report.resolve()),
            },
        }
        write_json(args.output, report)
    except (LifecycleCheckError, OSError, ValueError) as exc:
        parser.error(str(exc))
    print(
        "embedded lifecycle evidence: pass "
        "(shutdown/recovery plus 10 startup fault points, host restored)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
