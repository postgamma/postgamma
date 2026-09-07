"""Tests for the chunked results bounded tuple-streaming evidence gate."""

from __future__ import annotations

import sys
import unittest
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(PROJECT_ROOT / "buildsys"))

import check_chunked_results  # noqa: E402


def case_output() -> str:
    return "\n".join(
        "POSTGAMMA_STREAMING_CASE "
        f"name={name} sequence={sequence} kind={kind} rows={rows} "
        f"columns={columns} format={result_format} first= last= command= sqlstate="
        for name, sequence, kind, rows, columns, result_format in (
            check_chunked_results.EXPECTED_CASES
        )
    )


def candidate_marker(*, rss_delta_bytes: int = 4096) -> str:
    common = " ".join(
        f"{name}={value}"
        for name, value in check_chunked_results.COMMON_MARKER_VALUES.items()
    )
    candidate = " ".join(
        f"{name}={value}"
        for name, value in (
            check_chunked_results.CANDIDATE_ONLY_MARKER_VALUES.items()
        )
    )
    return (
        f"{check_chunked_results.CANDIDATE_MARKER} {common} {candidate} "
        f"rss_delta_bytes={rss_delta_bytes}\n"
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


class ChunkedResultsEvidenceTests(unittest.TestCase):
    def test_accepts_complete_streaming_sequence(self) -> None:
        cases = check_chunked_results.parse_cases(case_output())
        self.assertEqual(len(cases), 20)

    def test_rejects_missing_terminal_result(self) -> None:
        lines = case_output().splitlines()
        del lines[4]
        with self.assertRaisesRegex(
            check_chunked_results.ChunkedResultsCheckError,
            "streaming case sequence mismatch",
        ):
            check_chunked_results.parse_cases("\n".join(lines))

    def test_accepts_candidate_marker_with_bounded_rss(self) -> None:
        values = check_chunked_results.parse_marker(
            candidate_marker(),
            check_chunked_results.CANDIDATE_MARKER,
            candidate=True,
        )
        self.assertEqual(values["slow_rows"], "10000000")
        self.assertEqual(values["rss_delta_bytes"], "4096")

    def test_rejects_candidate_marker_over_rss_allowance(self) -> None:
        with self.assertRaisesRegex(
            check_chunked_results.ChunkedResultsCheckError,
            "exceeds allowance",
        ):
            check_chunked_results.parse_marker(
                candidate_marker(rss_delta_bytes=67_108_865),
                check_chunked_results.CANDIDATE_MARKER,
                candidate=True,
            )

    def test_accepts_clean_pooled_runtime(self) -> None:
        runtime = check_chunked_results.parse_runtime(valid_runtime())
        self.assertEqual(runtime["host_pid"], 1234)
        self.assertEqual(runtime["execution_token_budget"], 4)

    def test_rejects_runtime_without_quantum_yield(self) -> None:
        runtime = valid_runtime().replace("quantum_yields=16", "quantum_yields=0")
        with self.assertRaisesRegex(
            check_chunked_results.ChunkedResultsCheckError,
            "resumable quantums",
        ):
            check_chunked_results.parse_runtime(runtime)


if __name__ == "__main__":
    unittest.main()
