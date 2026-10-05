"""Tests for the declarative embedded SQL conformance gate."""

from __future__ import annotations

import copy
import json
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "buildsys"))

import check_embedded_conformance  # noqa: E402
import generate_embedded_conformance_cases  # noqa: E402


CORPUS_PATH = ROOT / "manifests/tests/embedded-conformance-v1.json"


class EmbeddedConformanceTests(unittest.TestCase):
    def test_real_corpus_is_broad_and_generates_deterministically(self) -> None:
        corpus = generate_embedded_conformance_cases.load_corpus(CORPUS_PATH)
        self.assertGreaterEqual(len(corpus["cases"]), 50)
        self.assertEqual(
            set(corpus["required_categories"]),
            {case["category"] for case in corpus["cases"]},
        )
        first = generate_embedded_conformance_cases.render_header(corpus)
        second = generate_embedded_conformance_cases.render_header(corpus)
        self.assertEqual(first, second)
        self.assertIn("postgamma_conformance_cases[]", first)

    def test_rejects_a_case_removed_below_the_coverage_floor(self) -> None:
        corpus = json.loads(CORPUS_PATH.read_text(encoding="utf-8"))
        corpus["cases"] = corpus["cases"][:20]
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "corpus.json"
            path.write_text(json.dumps(corpus), encoding="utf-8")
            with self.assertRaisesRegex(
                generate_embedded_conformance_cases.ConformanceCorpusError,
                "at least 40",
            ):
                generate_embedded_conformance_cases.load_corpus(path)

    def test_rejects_duplicate_case_names(self) -> None:
        corpus = json.loads(CORPUS_PATH.read_text(encoding="utf-8"))
        corpus["cases"][1]["name"] = corpus["cases"][0]["name"]
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "corpus.json"
            path.write_text(json.dumps(corpus), encoding="utf-8")
            with self.assertRaisesRegex(
                generate_embedded_conformance_cases.ConformanceCorpusError,
                "duplicate case name",
            ):
                generate_embedded_conformance_cases.load_corpus(path)

    def test_rejects_error_without_sqlstate(self) -> None:
        corpus = json.loads(CORPUS_PATH.read_text(encoding="utf-8"))
        error_case = next(
            case for case in corpus["cases"] if case["expected_kind"] == "error"
        )
        error_case.pop("expected_sqlstate")
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "corpus.json"
            path.write_text(json.dumps(corpus), encoding="utf-8")
            with self.assertRaisesRegex(
                generate_embedded_conformance_cases.ConformanceCorpusError,
                "requires a five-character SQLSTATE",
            ):
                generate_embedded_conformance_cases.load_corpus(path)

    def test_parser_requires_every_manifest_case_in_order(self) -> None:
        corpus = generate_embedded_conformance_cases.load_corpus(CORPUS_PATH)
        lines = []
        for expected in check_embedded_conformance.expected_cases(corpus):
            lines.append(
                "POSTGAMMA_EMBEDDED_CONFORMANCE_CASE "
                + " ".join(f"{name}={value}" for name, value in expected.items())
                + " rows=1 columns=1 digest=0123456789abcdef"
            )
        parsed = check_embedded_conformance.parse_cases("\n".join(lines), corpus)
        self.assertEqual(len(parsed), len(corpus["cases"]))
        altered = copy.copy(lines)
        altered[0], altered[1] = altered[1], altered[0]
        with self.assertRaisesRegex(
            check_embedded_conformance.EmbeddedConformanceError,
            "field ordinal",
        ):
            check_embedded_conformance.parse_cases("\n".join(altered), corpus)


if __name__ == "__main__":
    unittest.main()
