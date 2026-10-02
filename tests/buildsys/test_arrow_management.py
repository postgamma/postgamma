"""Tests for the Arrow management Arrow and management evidence gate."""

from __future__ import annotations

import sys
import tempfile
import unittest
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(PROJECT_ROOT / "buildsys"))

import check_arrow_management  # noqa: E402


def marker_line(marker: str, values: dict[str, str]) -> str:
    return marker + " " + " ".join(
        f"{name}={value}" for name, value in values.items()
    ) + "\n"


def valid_runtime() -> str:
    return (
        "LOG: POSTGAMMA_RUNTIME backend_model=thread host_pid=1234 "
        "provider=pooled threads_started=true role_process_launches=0 "
        "forbidden_process_launch_attempts=0 unsupported_role_requests=0 "
        "role_threads_active=0 runnable_sessions=0 running_quantums=0 "
        "pinned_sessions=0 blocked_sessions=0 execution_tokens_active=0 "
        "execution_token_rejections=0 role_completions=2 "
        "pooled_worker_threads=4 execution_token_budget=4 "
        "execution_tokens_peak=1\n"
    )


class ArrowManagementEvidenceTests(unittest.TestCase):
    def test_accepts_arrow_marker(self) -> None:
        values = check_arrow_management.parse_marker(
            marker_line(
                check_arrow_management.ARROW_MARKER,
                check_arrow_management.ARROW_VALUES,
            ),
            check_arrow_management.ARROW_MARKER,
            check_arrow_management.ARROW_VALUES,
        )
        self.assertEqual(values["formats"], "text,binary")

    def test_rejects_hidden_management_connection(self) -> None:
        output = marker_line(
            check_arrow_management.MANAGEMENT_MARKER,
            {
                **check_arrow_management.MANAGEMENT_VALUES,
                "hidden_connections": "1",
            },
        )
        with self.assertRaisesRegex(
            check_arrow_management.ArrowManagementCheckError,
            "hidden_connections",
        ):
            check_arrow_management.parse_marker(
                output,
                check_arrow_management.MANAGEMENT_MARKER,
                check_arrow_management.MANAGEMENT_VALUES,
            )

    def test_accepts_quiescent_runtime(self) -> None:
        runtime = check_arrow_management.parse_runtime(
            valid_runtime(), minimum_completions=2
        )
        self.assertEqual(runtime["pooled_worker_threads"], 4)

    def test_rejects_active_role_after_close(self) -> None:
        with self.assertRaisesRegex(
            check_arrow_management.ArrowManagementCheckError,
            "role_threads_active",
        ):
            check_arrow_management.parse_runtime(
                valid_runtime().replace(
                    "role_threads_active=0", "role_threads_active=1"
                ),
                minimum_completions=2,
            )

    def test_current_management_bridge_uses_native_checkpoint_state(self) -> None:
        report = check_arrow_management.audit_management_boundary(
            PROJECT_ROOT / "embedded-c/src/postgamma_management.c",
            PROJECT_ROOT
            / "runtime/include/postgamma/postgres_checkpoint_runtime_impl.h",
            PROJECT_ROOT / "runtime/src/instance_runtime.c",
            PROJECT_ROOT / "manifests/postgresql/adapter.json",
        )
        self.assertEqual(report["hidden_sql_entrypoints"], 0)
        self.assertEqual(report["semantic_observation_hooks"], 3)

    def test_rejects_management_through_hidden_sql(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary) / "management.c"
            source.write_text("void f(void) { pgm_connection_open(); }\n")
            with self.assertRaisesRegex(
                check_arrow_management.ArrowManagementCheckError,
                "forbidden execution path",
            ):
                check_arrow_management.audit_management_boundary(
                    source,
                    PROJECT_ROOT
                    / "runtime/include/postgamma/postgres_checkpoint_runtime_impl.h",
                    PROJECT_ROOT / "runtime/src/instance_runtime.c",
                    PROJECT_ROOT / "manifests/postgresql/adapter.json",
                )


if __name__ == "__main__":
    unittest.main()
