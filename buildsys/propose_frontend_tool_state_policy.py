#!/usr/bin/env python3
"""Create a review candidate for one PostgreSQL frontend/tool state catalog.

This command is intentionally not a normal build target. Its classifications
encode the reviewed PostgreSQL 19 baseline and fail on every unknown object.
The checked-in expanded policy, not this proposal command, is the build input.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import Any

from check_frontend_tool_state_policy import POLICY_KIND, FrontendToolStatePolicyError
from state_policy import catalog_candidates, load_json


IMMUTABLE_IDS = {
    "external:X86Features",
    "external:pg_enc2gettext_tbl",
    "external:pg_popcount_aarch64_dummy_variable",
    "function_static:src/common/md5_common.c:src/common/md5_common.c:bytesToHex:hex",
    "internal:src/common/instr_time.c:src/common/instr_time.c::tsc_info",
    "internal:src/interfaces/libpq/fe-connect.c:src/interfaces/libpq/fe-connect.c::supported_sasl_mechs",
}

SYNCHRONIZED_IDS = {
    "external:max_ticks_no_overflow": "pthread_once publishes immutable timing calibration",
    "external:pg_comp_crc32c": "pthread_once publishes the selected CRC implementation",
    "external:pg_g_threadlock": "libpq's thread-lock setter and users are serialized by the registered lock contract",
    "external:pg_popcount_masked_optimized": "pthread_once publishes the selected popcount implementation",
    "external:pg_popcount_optimized": "pthread_once publishes the selected popcount implementation",
    "external:ticks_per_ns_scaled": "pthread_once publishes immutable timing calibration",
    "external:timing_clock_source": "pthread_once publishes immutable timing calibration",
    "external:timing_initialized": "pthread_once owns timing initialization and publication",
    "external:timing_tsc_enabled": "pthread_once publishes immutable timing calibration",
    "external:timing_tsc_frequency_khz": "pthread_once publishes immutable timing calibration",
    "function_static:src/interfaces/libpq/fe-connect.c:src/interfaces/libpq/fe-connect.c:default_threadlock:singlethread_lock": "the pthread mutex is the synchronization primitive itself",
    "function_static:src/timezone/localtime.c:src/timezone/localtime.c:gmtsub:gmtptr": "pthread_once constructs and publishes the immutable GMT state",
}

CONNECTION_IDS = {
    "function_static:src/common/unicode_norm.c:src/common/unicode_norm.c:get_code_decomposition:x",
    "internal:src/interfaces/libpq/fe-exec.c:src/interfaces/libpq/fe-exec.c::static_client_encoding",
    "internal:src/interfaces/libpq/fe-exec.c:src/interfaces/libpq/fe-exec.c::static_std_strings",
}

FORBIDDEN_IDS = {
    "external:PQauthDataHook": "use the PostGamma host callback registry; private libpq must not expose the process-global hook setter",
    "external:pg_global_prng_state": "use instance, tool, or connection-owned PRNG state",
    "function_static:src/interfaces/libpq/fe-exec.c:src/interfaces/libpq/fe-exec.c:PQoidStatus:buf": "copy command status into result-owned storage and use numeric OID access",
    "function_static:src/port/strerror.c:src/port/strerror.c:pg_strerror:errorstr_buf": "use caller-owned pg_strerror_r storage",
    "internal:src/port/pqsignal.c:src/port/pqsignal.c::pqsignal_handlers": "use host-neutral callback dispatch without installing or retaining process signal handlers",
    "internal:src/timezone/localtime.c:src/timezone/localtime.c::tm": "use caller or execution-context-owned pg_tm storage",
}


def _is_json_table(identifier: str, path: str) -> bool:
    if path != "src/common/jsonapi.c":
        return False
    return (
        "::JSON_PROD_" in identifier
        or identifier.endswith("::td_parser_table")
        or identifier.endswith("::failed_oom")
        or identifier.endswith("::failed_inc_oom")
    )


def classify(candidate: dict[str, Any]) -> dict[str, Any]:
    identifier = candidate["id"]
    path = candidate["definition_path"]
    if identifier in IMMUTABLE_IDS or _is_json_table(identifier, path):
        return {
            "id": identifier,
            "owner": "library_immutable",
            "rationale": "reviewed as read-only after static initialization",
        }
    if identifier in SYNCHRONIZED_IDS:
        return {
            "id": identifier,
            "owner": "library_synchronized",
            "rationale": "one process-wide value with an explicit publication contract",
            "synchronization": SYNCHRONIZED_IDS[identifier],
        }
    if identifier in CONNECTION_IDS:
        return {
            "id": identifier,
            "owner": "connection",
            "rationale": "mutable compatibility state must follow the active private libpq connection",
        }
    if identifier in FORBIDDEN_IDS:
        return {
            "id": identifier,
            "owner": "forbidden",
            "rationale": "the upstream process-global behavior is unsafe in an embedded host",
            "replacement": FORBIDDEN_IDS[identifier],
        }
    if (
        path.startswith("src/bin/initdb/")
        or path.startswith("src/bin/pg_dump/")
        or path == "src/timezone/zic.c"
        or path == "src/common/file_perm.c"
        or path == "src/common/logging.c"
        or path == "src/common/pg_lzcompress.c"
    ):
        return {
            "id": identifier,
            "owner": "tool",
            "rationale": "mutable state belongs to one transformed frontend tool invocation",
        }
    raise FrontendToolStatePolicyError(
        f"unclassified PostgreSQL 19 frontend/tool state: {identifier} ({path})"
    )


def propose(catalog: dict[str, Any]) -> dict[str, Any]:
    candidates = catalog_candidates(catalog, FrontendToolStatePolicyError)
    decisions = [
        classify(candidate)
        for _identifier, candidate in sorted(candidates.items())
    ]
    return {
        "schema_version": 1,
        "kind": POLICY_KIND,
        "policy": (
            "Every mutable frontend/tool object has a reviewed library, tool, "
            "connection, or forbidden owner. New source candidates require review."
        ),
        "decisions": decisions,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--catalog", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    try:
        document = propose(load_json(args.catalog, FrontendToolStatePolicyError))
    except FrontendToolStatePolicyError as exc:
        parser.error(str(exc))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(
        json.dumps(document, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    print(f"proposed {len(document['decisions'])} frontend/tool ownership decisions")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
