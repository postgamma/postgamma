"""Tests for the COPY streaming bounded COPY evidence gate."""

from __future__ import annotations

import sys
import unittest
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(PROJECT_ROOT / "buildsys"))

import check_copy_streaming  # noqa: E402


def case_output() -> str:
    return "\n".join(
        "POSTGAMMA_COPY_CASE "
        f"name={name} sequence={sequence} kind={kind} bytes={size} hash={digest}"
        for name, sequence, kind, size, digest in check_copy_streaming.EXPECTED_CASES
    )


def candidate_marker(
    *, early_accepted: int = 64, rss_delta_bytes: int = 4096
) -> str:
    required = " ".join(
        f"{name}={value}"
        for name, value in check_copy_streaming.CANDIDATE_MARKER_VALUES.items()
    )
    return (
        f"{check_copy_streaming.CANDIDATE_MARKER} {required} "
        "partial_writes=10 write_again=4 read_calls=20 "
        f"early_accepted={early_accepted} rss_delta_bytes={rss_delta_bytes}\n"
    )


def valid_runtime() -> str:
    return (
        "LOG: POSTGAMMA_RUNTIME backend_model=thread host_pid=1234 "
        "provider=pooled threads_started=true role_process_launches=0 "
        "forbidden_process_launch_attempts=0 unsupported_role_requests=0 "
        "role_threads_active=0 client_threads_started=4 pooled_worker_threads=4 "
        "client_quantums=20 quantum_yields=16 runnable_sessions=0 "
        "running_quantums=0 pinned_sessions=0 blocked_sessions=0 "
        "execution_tokens_active=0 execution_token_budget=4 "
        "execution_tokens_peak=2 execution_token_rejections=0\n"
    )


class CopyEvidenceTests(unittest.TestCase):
    def test_accepts_complete_copy_sequence(self) -> None:
        cases = check_copy_streaming.parse_cases(case_output())
        self.assertEqual(len(cases), 4)

    def test_rejects_copy_byte_mismatch(self) -> None:
        output = case_output().replace("bytes=262163", "bytes=262162", 1)
        with self.assertRaisesRegex(
            check_copy_streaming.CopyCheckError, "COPY case sequence mismatch"
        ):
            check_copy_streaming.parse_cases(output)

    def test_accepts_complete_candidate_matrix(self) -> None:
        values = check_copy_streaming.parse_marker(
            candidate_marker(), check_copy_streaming.CANDIDATE_MARKER, candidate=True
        )
        self.assertEqual(values["early_accepted"], "64")
        self.assertEqual(values["concurrent_owner_busy"], "true")

    def test_rejects_error_after_all_input_was_accepted(self) -> None:
        with self.assertRaisesRegex(
            check_copy_streaming.CopyCheckError, "did not stop COPY input early"
        ):
            check_copy_streaming.parse_marker(
                candidate_marker(early_accepted=1_048_576),
                check_copy_streaming.CANDIDATE_MARKER,
                candidate=True,
            )

    def test_rejects_rss_over_allowance(self) -> None:
        with self.assertRaisesRegex(
            check_copy_streaming.CopyCheckError, "exceeds allowance"
        ):
            check_copy_streaming.parse_marker(
                candidate_marker(rss_delta_bytes=67_108_865),
                check_copy_streaming.CANDIDATE_MARKER,
                candidate=True,
            )

    def test_accepts_clean_pooled_runtime(self) -> None:
        runtime = check_copy_streaming.parse_runtime(valid_runtime())
        self.assertEqual(runtime["host_pid"], 1234)
        self.assertEqual(runtime["execution_token_budget"], 4)

    def test_rejects_runtime_without_quantum_yield(self) -> None:
        runtime = valid_runtime().replace("quantum_yields=16", "quantum_yields=0")
        with self.assertRaisesRegex(
            check_copy_streaming.CopyCheckError, "resumable quantums"
        ):
            check_copy_streaming.parse_runtime(runtime)


if __name__ == "__main__":
    unittest.main()
