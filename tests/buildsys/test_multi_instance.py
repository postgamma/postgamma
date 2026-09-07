"""Tests for the multi-instance runtime dual-live instance evidence gate."""

from __future__ import annotations

import sys
import unittest
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(PROJECT_ROOT / "buildsys"))

import check_multi_instance  # noqa: E402


def marker(capability: str = "true") -> str:
    return (
        "POSTGAMMA_RELEASE_MULTI_INSTANCE host_pid=42 clusters=2 "
        "instances_opened=3 instances_live_peak=2 connections_opened=9 "
        "concurrent_connections=8 distinct_system_identifiers=true "
        "data_isolation=true guc_isolation=true temp_isolation=true "
        "notice_routing=true notification_routing=true "
        "simultaneous_active=true parallel_query=true "
        "cancel_peer_in_flight=true cancel_isolation=true "
        "checkpoints=2 first_closed=true "
        "first_reopened=true virtual_pids_valid=true "
        "extension_sdk=true bundled_extensions=2 vector_extension=true "
        "vector_hnsw=true vector_ivfflat=true vector_guc_isolation=true "
        "vector_concurrency=true vector_parallel_query=true "
        "vector_persistence=true "
        "extension_instances=3 extension_sessions=9 "
        "extension_session_resets=2 "
        "extension_session_mobility=true extension_parallel_workers=true "
        "extension_descriptor_rejections=17 "
        "extension_resources=true extension_failure_recovered=true "
        "unbundled_rejected=true "
        f"capability_advertised={capability} phase=closed\n"
    )


def runtime(
    parallel_threads: int,
    role_completions: int = 11,
    carrier_migrations: int = 3,
) -> str:
    return (
        "LOG: POSTGAMMA_RUNTIME backend_model=thread host_pid=42 "
        "provider=pooled threads_started=true role_process_launches=0 "
        "forbidden_process_launch_attempts=0 unsupported_role_requests=0 "
        "parallel_threads_started="
        f"{parallel_threads} role_completions={role_completions} role_threads_active=0 "
        f"carrier_migrations={carrier_migrations} "
        "pooled_worker_threads=4 runnable_sessions=0 running_quantums=0 "
        "pinned_sessions=0 blocked_sessions=0 execution_tokens_active=0 "
        "execution_tokens_peak=2 execution_token_budget=4 "
        "execution_token_rejections=0\n"
    )


def expected_startup_failure() -> str:
    return (
        'LOG: FATAL:  bundled extension "postgamma_sdk_probe" failed during '
        "instance startup\n"
    )


def extension_lifecycle(shutdowns: tuple[int, ...] = (1, 1, 1)) -> str:
    lines = [
        "POSTGAMMA_EXTENSION_LIBRARY id=postgamma_sdk_probe initializations=1",
        "POSTGAMMA_EXTENSION_LIBRARY id=pgvector initializations=1",
    ]
    lines.extend(
        "POSTGAMMA_EXTENSION "
        f"generation={index + 1} id=postgamma_sdk_probe "
        "instance_requests=1 instance_startups=1 "
        f"instance_shutdowns={shutdown} phase=closed"
        for index, shutdown in enumerate(shutdowns)
    )
    lines.extend(
        "POSTGAMMA_EXTENSION "
        f"generation={index + 1} id=pgvector "
        "instance_requests=1 instance_startups=1 "
        "instance_shutdowns=1 phase=closed"
        for index in range(3)
    )
    lines.extend(
        "POSTGAMMA_EXTENSION_SESSION generation=1 id=postgamma_sdk_probe "
        f"connection={index + 1} phase={phase}"
        for phase in ("initialized", "destroyed")
        for index in range(9)
    )
    return "\n".join(lines) + "\n"


def state_runtime(slot_ids: tuple[str, ...] = ("required",)) -> dict[str, object]:
    return {
        "kind": "postgamma.backend-state-runtime",
        "owner_bytes": {"instance": 8},
        "owner_slots": {"instance": len(slot_ids)},
        "slots": [
            {
                "id": identifier,
                "owner": "instance",
                "offset": index * 4,
                "size": 4,
            }
            for index, identifier in enumerate(slot_ids)
        ],
    }


def state_policy() -> dict[str, object]:
    return {
        "kind": "postgamma.backend-state-ownership",
        "decisions": [
            {"id": "required", "owner": "instance"},
            {
                "id": "conditional",
                "owner": "instance",
                "availability": "conditional",
            },
        ],
    }


class ReleaseMultiInstanceTests(unittest.TestCase):
    def test_accepts_reviewed_instance_state_facts(self) -> None:
        facts = check_multi_instance.validate_instance_state_facts(
            state_runtime(("required", "conditional")), state_policy()
        )
        self.assertEqual(facts["slots"], 2)
        self.assertEqual(facts["slot_ids"], ["conditional", "required"])

    def test_rejects_a_missing_required_instance_slot(self) -> None:
        with self.assertRaisesRegex(
            check_multi_instance.MultiInstanceCheckError,
            "omits required",
        ):
            check_multi_instance.validate_instance_state_facts(
                state_runtime(("conditional",)), state_policy()
            )

    def test_rejects_instance_slot_count_drift(self) -> None:
        runtime_facts = state_runtime()
        runtime_facts["owner_slots"] = {"instance": 2}
        with self.assertRaisesRegex(
            check_multi_instance.MultiInstanceCheckError,
            "size or slot count",
        ):
            check_multi_instance.validate_instance_state_facts(
                runtime_facts, state_policy()
            )

    def test_parses_complete_dual_live_proof(self) -> None:
        parsed_marker = check_multi_instance.parse_marker(marker())
        parsed_runtimes = check_multi_instance.parse_runtimes(
            runtime(2) + runtime(0) + runtime(4) + expected_startup_failure(),
            int(parsed_marker["host_pid"]),
        )
        self.assertEqual(parsed_marker["instances_live_peak"], "2")
        self.assertEqual(len(parsed_runtimes), 3)

    def test_rejects_missing_capability_advertisement(self) -> None:
        with self.assertRaisesRegex(
            check_multi_instance.MultiInstanceCheckError,
            "capability_advertised",
        ):
            check_multi_instance.parse_marker(marker("false"))

    def test_rejects_missing_reopened_runtime(self) -> None:
        with self.assertRaisesRegex(
            check_multi_instance.MultiInstanceCheckError,
            "expected three",
        ):
            check_multi_instance.parse_runtimes(
                runtime(2) + runtime(4) + expected_startup_failure(), 42
            )

    def test_rejects_parallel_workers_from_only_one_instance(self) -> None:
        with self.assertRaisesRegex(
            check_multi_instance.MultiInstanceCheckError,
            "fewer than two",
        ):
            check_multi_instance.parse_runtimes(
                runtime(2) + runtime(0) + runtime(0) + expected_startup_failure(),
                42,
            )

    def test_rejects_session_mobility_without_two_migrating_instances(self) -> None:
        with self.assertRaisesRegex(
            check_multi_instance.MultiInstanceCheckError,
            "migrated sessions",
        ):
            check_multi_instance.parse_runtimes(
                runtime(2, carrier_migrations=2)
                + runtime(2, carrier_migrations=0)
                + runtime(0, carrier_migrations=0)
                + expected_startup_failure(),
                42,
            )

    def test_parses_measured_extension_lifecycle_counts(self) -> None:
        lifecycle = check_multi_instance.parse_extension_lifecycle(
            extension_lifecycle()
        )
        self.assertEqual(lifecycle["instance_requests"], 6)
        self.assertEqual(lifecycle["instance_startups"], 6)
        self.assertEqual(lifecycle["instance_shutdowns"], 6)

    def test_rejects_unpaired_extension_shutdown(self) -> None:
        with self.assertRaisesRegex(
            check_multi_instance.MultiInstanceCheckError,
            "lifecycle is incomplete",
        ):
            check_multi_instance.parse_extension_lifecycle(
                extension_lifecycle((1, 0, 1))
            )


if __name__ == "__main__":
    unittest.main()
