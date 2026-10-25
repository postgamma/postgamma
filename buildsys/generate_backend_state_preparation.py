#!/usr/bin/env python3
"""Generate an explicit preparation registry from a qualified native set."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import re

from plan_backend_state_preparation import digest
from prepare_backend_state_templates import provider_name
from qualify_backend_state_preparation import verify_inputs, preparation_set_identity


def emit(report: dict) -> str:
    if (report.get("kind") != "postgamma.backend-state-preparation-qualified" or
            report.get("scope") != "audited_native_server_configuration"):
        raise ValueError("registry requires native artifact qualification")
    identity = {k: v for k, v in report.items() if k not in {"identity", "evidence"}}
    if digest(identity) != report.get("identity"):
        raise ValueError("preparation qualification identity changed")
    if report.get("set_identity") != preparation_set_identity(report):
        raise ValueError("preparation set identity changed")
    if not re.fullmatch(r"[0-9a-f]{64}", report["layout_sha256"]):
        raise ValueError("invalid preparation layout identity")
    entries = [slot for slot in report["slots"] if slot["preparation_eligible"]]
    if not entries or len({s["index"] for s in entries}) != len(entries):
        raise ValueError("empty or duplicate preparation set")
    entries.sort(key=lambda slot: slot["index"])
    by_id = {s["identifier"]: s for s in entries}
    lines = ["/* Generated from the audited native preparation set; do not edit. */",
             "#ifdef POSTGAMMA_DIRECT_ROLE_STATE"]
    for index in range(4):
        word = report["set_identity"][index * 16:(index + 1) * 16]
        lines.append(f'_Static_assert(POSTGAMMA_DIRECT_ROLE_SET_{index} == UINT64_C(0x{word}), '
                     '"direct access and preparation sets differ");')
    lines.append("#endif")
    for entry in entries:
        if entry["owner"] != "role" or entry["fallback_reasons"]:
            raise ValueError("unqualified state in preparation set")
        provider = entry["provider"]
        if provider is not None:
            if provider != provider_name(entry["identifier"]):
                raise ValueError("provider identity does not match slot")
            lines.append(f"extern const volatile void *{provider}(void);")
        elif entry["initialization"] == "copy_template":
            raise ValueError("copy template has no provider")
        targets = []
        for target in entry["relocations"]:
            if target not in by_id or not by_id[target]["provider"]:
                raise ValueError("preparation relocation set is not closed")
            targets.append(by_id[target]["enum_name"])
        if targets:
            lines.extend([f"static const PostgammaBackendStateSlot PostgammaPreparedTargets_{entry['index']}[] = {{",
                          "\t" + ", ".join(targets), "};"])
    lines.extend(["static const PostgammaBackendStatePreparationEntry PostgammaPreparedEntries[] =", "{"])
    for entry in entries:
        count = len(entry["relocations"])
        targets = f"PostgammaPreparedTargets_{entry['index']}" if count else "NULL"
        provider = entry["provider"] or "NULL"
        zero = "false" if entry["initialization"] == "copy_template" else "true"
        lines.append(f"\t{{{entry['enum_name']}, {provider}, {targets}, {count}, {zero}}},")
    lines.extend(["};", "static const PostgammaBackendStatePreparation PostgammaPreparedRoleState =", "{",
                  f'\t"{report["set_identity"]}",', f'\t"{report["layout_sha256"]}",',
                  "\tPostgammaPreparedEntries,",
                  "\tsizeof(PostgammaPreparedEntries) / sizeof(PostgammaPreparedEntries[0])", "};", ""])
    return "\n".join(lines)


def emit_direct(report: dict) -> str:
    # Validate the same proof used by the initializer registry.
    emit(report)
    lines = ["/* Generated from the audited preparation set; do not edit. */",
             "#ifndef POSTGAMMA_BACKEND_STATE_DIRECT_H",
             "#define POSTGAMMA_BACKEND_STATE_DIRECT_H"]
    for index in range(4):
        word = report["set_identity"][index * 16:(index + 1) * 16]
        lines.append(f"#define POSTGAMMA_DIRECT_ROLE_SET_{index} UINT64_C(0x{word})")
    # Mixed objects from different sets must fail to link, not share a wrong view.
    lines.extend(["#define PostgammaCurrentPreparedRoleState " +
                  "PostgammaPreparedRoleState_" + report["set_identity"],
                  "static inline __attribute__((always_inline)) size_t",
                  "postgamma_backend_state_prepared_offset(PostgammaBackendStateSlot slot)",
                  "{", "\tswitch (slot)", "\t{"])
    for entry in sorted(report["slots"], key=lambda slot: slot["index"]):
        if entry["preparation_eligible"]:
            lines.append(f"\t\tcase {entry['enum_name']}: return {entry['offset']};")
    lines.extend(["\t\tdefault: return SIZE_MAX;", "\t}", "}", "#endif", ""])
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--qualification", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--direct-header", type=Path)
    args = parser.parse_args()
    try:
        report = json.loads(args.qualification.read_text())
        verify_inputs(report["evidence"]["inputs"])
        content = emit(report)
        direct = emit_direct(report) if args.direct_header else None
    except (ValueError, OSError, KeyError, TypeError) as exc:
        parser.error(str(exc))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(content)
    if args.direct_header:
        args.direct_header.parent.mkdir(parents=True, exist_ok=True)
        args.direct_header.write_text(direct)
    print(f"preparation registry: {report['summary']['preparation_eligible']} role slots")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
