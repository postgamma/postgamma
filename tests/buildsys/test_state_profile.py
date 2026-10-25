"""Keep profiling attribution conservative and avoid inclusive double counting."""

from __future__ import annotations

import importlib.util
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    "state_profile", ROOT / "tests/performance/analyze_state_access.py"
)
profile = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(profile)


class StateProfileTests(unittest.TestCase):
    def test_calls_and_branch_associations_do_not_duplicate_self_cost(self):
        result = profile.read_profile(
            "positions: instr line\nevents: Ir\nsummary: 30\n"
            "ob=/example/postgres\nfn=caller\n0x100 0 10\n"
            "cfn=callee\ncalls=10 0x200 0\n0x100 0 20\n"
            "jcnd=5/10 0x110 0\n0x100 0\n"
            "fn=callee\n0x200 0 20\ntotals: 30\n"
        )
        self.assertEqual(result["total"], 30)
        self.assertEqual(sum(result["costs"].values()), 30)
        self.assertEqual(result["calls"]["callee"], 10)

    def test_incomplete_or_compressed_profiles_fail_closed(self):
        prefix = "positions: instr line\nevents: Ir\n"
        for body in ("ob=(1) /example\n", "fn=(1) palloc\n",
                     "ob=/example\nfn=caller\n+1 0 10\n",
                     "ob=/example\nfn=caller\n0x100 0 10\ntotals: 11\n"):
            with self.subTest(body=body), self.assertRaises(ValueError):
                profile.read_profile(prefix + body)

    @staticmethod
    def body(tail):
        return [
            (0x100, "mov", "rbx,QWORD PTR [rip+0x10] # " + profile.TLS_NAME),
            (0x108, "mov", "rax,QWORD PTR fs:[rbx]"),
            *tail,
        ]

    def test_slot_offset_is_counted_once_with_its_own_check(self):
        classified = profile.classify(self.body([
            (0x110, "mov", "rax,QWORD PTR [rax+0xe0]"),
            (0x118, "test", "rax,rax"),
            (0x11a, "je", "130 <resolver>"),
            (0x120, "mov", "rax,QWORD PTR [rax]"),
            (0x128, "ret", ""),
        ]))
        self.assertEqual(classified[0x110], ("slot_load", 28))
        self.assertEqual(classified[0x118], ("slot_test", None))
        self.assertEqual(classified[0x11a], ("slot_branch", None))
        self.assertNotIn(0x120, classified)

    def test_conflicting_control_flow_cannot_invent_an_access(self):
        classified = profile.classify(self.body([
            (0x110, "je", "120 <merge>"),
            (0x118, "mov", "rax,rdi"),
            (0x120, "mov", "rax,QWORD PTR [rax+0xe0]"),
            (0x128, "ret", ""),
        ]))
        self.assertNotIn(0x120, classified)

    def test_calls_and_implicit_writes_invalidate_volatile_register_facts(self):
        for op, operand in (("call", "500 <external>"), ("mul", "rdi"),
                            ("xchg", "rdi,rax"), ("mov", "eax,edi")):
            with self.subTest(op=op):
                classified = profile.classify(self.body([
                    (0x110, op, operand),
                    (0x118, "mov", "rax,QWORD PTR [rax+0xe0]"),
                    (0x120, "ret", ""),
                ]))
                self.assertNotIn(0x118, classified)

    def test_dynamic_slot_indexes_are_not_assigned_a_constant_slot(self):
        classified = profile.classify(self.body([
            (0x110, "mov", "rax,QWORD PTR [rax+rdx*8]"),
            (0x118, "ret", ""),
        ]))
        self.assertEqual(classified[0x110], ("dynamic_slot_load", None))

    def test_local_exec_requires_the_explicit_symbol_offset(self):
        body = [(0x100, "mov", "rax,QWORD PTR fs:0xffffffffffffffe0"),
                (0x108, "mov", "rax,QWORD PTR [rax+0x10]"),
                (0x110, "ret", "")]
        self.assertEqual(profile.classify(body), {})
        classified = profile.classify(body, 0xffffffffffffffe0)
        self.assertEqual(classified[0x108], ("slot_load", 2))


if __name__ == "__main__":
    unittest.main()
