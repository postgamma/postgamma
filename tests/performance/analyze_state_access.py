#!/usr/bin/env python3
"""Conservative instruction-level state access attribution for ELF x86-64.

Use Callgrind --dump-instr=yes --compress-strings=no --compress-pos=no.
Only self costs are summed: call-edge inclusive costs must never be added.
Register facts are intersected at control-flow joins; unknown indirect jumps
and dynamic slot indexes remain unclassified. This is a lower bound on state
access costs, not a proof that other instructions are unrelated to state.
"""
from __future__ import annotations

import argparse
from collections import Counter, deque
from dataclasses import asdict
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "buildsys"))
from generate_backend_state_runtime import compile_slots  # noqa: E402

TLS_NAME = "PostgammaCurrentBackendStateAddresses"
CALLER_SAVED = {"rax", "rcx", "rdx", "rsi", "rdi", "r8", "r9", "r10", "r11"}


def read_profile(text: str) -> dict:
    if "positions: instr line\n" not in text or "events: Ir\n" not in text:
        raise ValueError("expected instruction/line positions and only Ir events")
    costs = Counter()
    calls = Counter()
    objects = Counter()
    obj = fn = target = ""
    association = False
    total = None
    for line in text.splitlines():
        if line.startswith(("fn=(", "ob=(", "cfn=(", "cob=(")):
            raise ValueError("compressed profile is not supported")
        if line.startswith("ob="):
            obj = line[3:]
        elif line.startswith("fn="):
            fn = line[3:]
        elif line.startswith("cfn="):
            target = line[4:]
        elif line.startswith("calls="):
            count = int(line[6:].split()[0])
            calls[target] += count
            association = True
        elif line.startswith(("jump=", "jcnd=")):
            association = True
        elif line.startswith("0x"):
            fields = line.split()
            if not association and len(fields) == 3:
                address, _, count = fields
                amount = int(count)
                costs[(obj, fn, int(address, 16))] += amount
                objects[obj] += amount
            association = False
        elif line.startswith("totals:"):
            total = int(line.split()[1])
        elif line and line[0] in "+-*":
            raise ValueError("compressed profile is not supported")
    if total is None or sum(costs.values()) != total:
        raise ValueError(f"self costs do not match totals: {sum(costs.values())} != {total}")
    return {"costs": costs, "calls": calls, "objects": objects, "total": total}


def register(name: str) -> str | None:
    if re.fullmatch(r"r(?:[abcd]x|[sb]p|[sd]i|[89]|1[0-5])", name):
        return name
    if re.fullmatch(r"e(?:[abcd]x|[sb]p|[sd]i)", name):
        return "r" + name[1:]
    if re.fullmatch(r"r(?:[89]|1[0-5])[dwb]", name):
        return name[:-1]
    aliases = {"ax": "rax", "al": "rax", "ah": "rax", "bx": "rbx", "bl": "rbx",
               "bh": "rbx", "cx": "rcx", "cl": "rcx", "ch": "rcx", "dx": "rdx",
               "dl": "rdx", "dh": "rdx", "si": "rsi", "sil": "rsi", "di": "rdi",
               "dil": "rdi", "bp": "rbp", "bpl": "rbp", "sp": "rsp", "spl": "rsp"}
    return aliases.get(name)


def disassembly(text: str) -> dict[str, list[tuple[int, str, str]]]:
    functions = {}
    body = None
    for line in text.splitlines():
        match = re.fullmatch(r"[0-9a-f]+ <(.+)>:", line)
        if match:
            body = functions.setdefault(match[1], [])
        else:
            match = re.match(r"\s*([0-9a-f]+):\s+(\S+)(?:\s+(.*))?$", line)
            if match and body is not None:
                body.append((int(match[1], 16), match[2], match[3] or ""))
    return functions


def step(instruction: tuple[int, str, str], incoming: dict, local_exec: int | None):
    _, op, operands = instruction
    facts = dict(incoming)
    code = operands.split("#", 1)[0].strip()
    args = [value.strip() for value in code.split(",")]
    destination = register(args[0]) if args else None
    source = args[1] if len(args) == 2 else ""
    category = None
    slot = None
    new = None
    # The exported symbol's GOT entry is an unambiguous TLS-offset source.
    if op == "mov" and TLS_NAME in operands and "[rip+" in source:
        new, category = ("tls-offset", 0), "tls_got"
    elif op == "mov" and "fs:" in source:
        match = re.search(r"fs:\[([a-z0-9]+)\]", source)
        direct = re.search(r"fs:(0x[0-9a-f]+)", source)
        if (match and incoming.get(match[1]) == ("tls-offset", 0)) or (
            direct and local_exec is not None and int(direct[1], 16) == local_exec
        ):
            new, category = ("table", 0), "tls_table"
    elif op == "mov" and source.startswith("QWORD PTR ["):
        match = re.fullmatch(r"QWORD PTR \[([a-z0-9]+)(?:\+(0x[0-9a-f]+))?\]", source)
        if match and incoming.get(match[1], (None,))[0] == "table":
            offset = incoming[match[1]][1] + int(match[2] or "0", 16)
            if offset >= 0 and offset % 8 == 0:
                slot, category = offset // 8, "slot_load"
                new = ("slot", slot)
        elif any(incoming.get(reg, (None,))[0] == "table"
                 for reg in re.findall(r"\b[a-z][a-z0-9]*\b", source)):
            category = "dynamic_slot_load"
    elif op == "mov" and register(source):
        # Narrow writes are not pointer copies, even when the register aliases.
        if args[0] == destination and source == register(source):
            new = incoming.get(register(source))
    elif op == "test" and len(args) == 2 and args[0] == args[1]:
        if incoming.get(destination, (None,))[0] == "slot":
            category = "slot_test"
    if op.startswith("call"):
        for reg in CALLER_SAVED:
            facts.pop(reg, None)
    elif op in {"cmp", "test", "push", "nop", "endbr64", "ret", "retq"} or op.startswith("j"):
        pass
    elif op in {"mov", "movzx", "movsx", "movsxd", "lea", "pop", "add", "sub",
                "and", "or", "xor", "shl", "shr", "sar", "sal", "inc", "dec",
                "neg", "not"} or op.startswith(("cmov", "set")):
        if destination:
            facts.pop(destination, None)
            if new is not None:
                facts[destination] = new
    else:
        # Implicit or multiple register writes (mul, div, xchg, string ops)
        # require a dedicated model before they can preserve pointer facts.
        facts.clear()
    return facts, category, slot


def classify(body: list[tuple[int, str, str]], local_exec: int | None = None) -> dict:
    if not body:
        return {}
    indexes = {item[0]: index for index, item in enumerate(body)}
    incoming = {0: {}}
    pending = deque([0])
    while pending:
        index = pending.popleft()
        _, op, operands = body[index]
        out, _, _ = step(body[index], incoming[index], local_exec)
        successors = []
        if op not in {"ret", "retq", "ud2", "jmp", "jmpq"} and index + 1 < len(body):
            successors.append(index + 1)
        if op.startswith("j"):
            target = re.match(r"([0-9a-f]+)\s+<", operands)
            if target and int(target[1], 16) in indexes:
                successors.append(indexes[int(target[1], 16)])
        for successor in successors:
            previous = incoming.get(successor)
            merged = out if previous is None else {
                reg: value for reg, value in previous.items() if out.get(reg) == value
            }
            if previous is None or previous != merged:
                incoming[successor] = dict(merged)
                pending.append(successor)
    classified = {}
    for index, facts in incoming.items():
        _, category, slot = step(body[index], facts, local_exec)
        if category:
            classified[body[index][0]] = (category, slot)
            # Count only the adjacent zero branch belonging to a proven slot
            # test. This deliberately misses reordered or shared checks.
            if category == "slot_test" and index + 1 < len(body) and body[index + 1][1] in {"je", "jne", "jz", "jnz"}:
                classified[body[index + 1][0]] = ("slot_branch", None)
    return classified


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--profile", type=Path, required=True)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--catalog", type=Path, required=True)
    parser.add_argument("--policy", type=Path, required=True)
    parser.add_argument("--ownership", type=Path, required=True)
    parser.add_argument("--local-exec-tpoff", type=lambda value: int(value, 0))
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    profile = read_profile(args.profile.read_text())
    slots, _, _ = compile_slots(*(json.loads(path.read_text()) for path in
                                  (args.catalog, args.policy, args.ownership)))
    assembly = subprocess.check_output(["objdump", "-d", "-M", "intel", "--no-show-raw-insn",
                                        str(args.binary)], text=True)
    categories = {}
    instructions = {}
    for name, body in disassembly(assembly).items():
        categories.update(classify(body, args.local_exec_tpoff))
        instructions.update({address: op for address, op, _ in body})
    slot_counts, attributed, function_costs, opcodes = Counter(), Counter(), Counter(), Counter()
    main_cost = matched = 0
    executable = args.binary.resolve()
    main_objects = {obj for obj in profile["objects"] if Path(obj).resolve() == executable}
    for (obj, function, address), cost in profile["costs"].items():
        if obj not in main_objects:
            continue
        main_cost += cost
        function_costs[function] += cost
        if address in instructions:
            matched += cost
            opcodes[instructions[address]] += cost
        if address in categories:
            category, slot = categories[address]
            attributed[category] += cost
            if slot is not None:
                if slot >= len(slots):
                    raise ValueError(f"attributed slot out of range: {slot}")
                slot_counts[slot] += cost
    if main_cost == 0 or matched != main_cost:
        raise ValueError(f"binary/profile instruction identity mismatch: {matched} != {main_cost}")
    report = {
        "binary_sha256": hashlib.sha256(args.binary.read_bytes()).hexdigest(),
        "profile_sha256": hashlib.sha256(args.profile.read_bytes()).hexdigest(),
        "profile_total_ir": profile["total"], "main_binary_ir": main_cost,
        "instruction_match_ir": matched, "attributed_ir_lower_bound": dict(attributed),
        "slot_accesses": [{**asdict(slots[slot]), "accesses": count}
                          for slot, count in slot_counts.most_common()],
        "functions": function_costs.most_common(), "opcodes": dict(opcodes),
        "objects": profile["objects"].most_common(),
        "runtime_calls": {name: count for name, count in profile["calls"].items()
                          if name.startswith("postgamma_")},
        "limitations": ["Only proven register flows and constant slot offsets are attributed.",
                        "Dynamic indexes and unknown indirect control flow are not assigned to slots.",
                        "Bridge calls are supplemental; do not add them to slot-load counts.",
                        "Register saves/restores are reported, not assumed to be caused by state access."]
    }
    args.output.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({key: report[key] for key in ("profile_total_ir", "main_binary_ir", "attributed_ir_lower_bound")}))


if __name__ == "__main__":
    main()
