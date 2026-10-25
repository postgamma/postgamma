#!/usr/bin/env python3
"""Keep one libpq session across externally controlled profiling intervals.

The JSON script is a list of simple-protocol statements, including explicit
BEGIN/COMMIT when required. Every iteration checks the complete ordered results
against the first iteration. The controller must also compare the initial
oracle across server variants; a self-comparison cannot establish correctness.
This is a profiling driver, not a throughput benchmark.
"""

from __future__ import annotations

import argparse
import ctypes
import json
import sys


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--libpq", required=True)
    parser.add_argument("--conninfo", required=True)
    parser.add_argument("--script", required=True)
    args = parser.parse_args()
    with open(args.script, encoding="utf-8") as stream:
        statements = json.load(stream)
    if not statements or any(not isinstance(sql, str) for sql in statements):
        parser.error("script must be a nonempty JSON list of SQL strings")

    pq = ctypes.CDLL(args.libpq)
    pointer, integer, string = ctypes.c_void_p, ctypes.c_int, ctypes.c_char_p
    signatures = {
        "PQconnectdb": (pointer, [string]),
        "PQstatus": (integer, [pointer]),
        "PQerrorMessage": (string, [pointer]),
        "PQbackendPID": (integer, [pointer]),
        "PQfinish": (None, [pointer]),
        "PQexec": (pointer, [pointer, string]),
        "PQclear": (None, [pointer]),
        "PQresultStatus": (integer, [pointer]),
        "PQresultErrorMessage": (string, [pointer]),
        "PQcmdStatus": (string, [pointer]),
        "PQntuples": (integer, [pointer]),
        "PQnfields": (integer, [pointer]),
        "PQfname": (string, [pointer, integer]),
        "PQftype": (ctypes.c_uint, [pointer, integer]),
        "PQfmod": (integer, [pointer, integer]),
        "PQfformat": (integer, [pointer, integer]),
        "PQgetisnull": (integer, [pointer, integer, integer]),
        "PQgetlength": (integer, [pointer, integer, integer]),
        "PQgetvalue": (pointer, [pointer, integer, integer]),
    }
    for name, (result, arguments) in signatures.items():
        function = getattr(pq, name)
        function.restype, function.argtypes = result, arguments

    connection = pq.PQconnectdb(args.conninfo.encode())
    try:
        if not connection or pq.PQstatus(connection) != 0:
            raise RuntimeError(pq.PQerrorMessage(connection))

        def execute(sql: str) -> dict:
            result = pq.PQexec(connection, sql.encode())
            if not result:
                raise RuntimeError(pq.PQerrorMessage(connection))
            try:
                status = pq.PQresultStatus(result)
                if status not in (1, 2):  # PGRES_COMMAND_OK, PGRES_TUPLES_OK
                    raise RuntimeError(pq.PQresultErrorMessage(result))
                fields = range(pq.PQnfields(result))
                columns = [
                    [pq.PQfname(result, col).hex(), pq.PQftype(result, col),
                     pq.PQfmod(result, col), pq.PQfformat(result, col)]
                    for col in fields
                ]
                rows = []
                for row in range(pq.PQntuples(result)):
                    rows.append([
                        None if pq.PQgetisnull(result, row, col) else
                        ctypes.string_at(pq.PQgetvalue(result, row, col),
                                         pq.PQgetlength(result, row, col)).hex()
                        for col in fields
                    ])
                return {"status": status, "command": pq.PQcmdStatus(result).hex(),
                        "columns": columns, "rows": rows}
            finally:
                pq.PQclear(result)

        oracle = [execute(sql) for sql in statements]
        print(json.dumps({"backend_pid": pq.PQbackendPID(connection),
                          "oracle": oracle}), flush=True)
        for line in sys.stdin:
            if line.startswith("{"):
                command = json.loads(line)
                print(json.dumps({"control": execute(command["sql"])}), flush=True)
                continue
            count = int(line)
            if count == 0:
                break
            if count < 0:
                raise ValueError("transaction count must be positive")
            for iteration in range(count):
                for index, sql in enumerate(statements):
                    actual = execute(sql)
                    if actual != oracle[index]:
                        raise RuntimeError(
                            f"result changed at transaction {iteration}, "
                            f"statement {index}: {actual!r} != {oracle[index]!r}"
                        )
            print(json.dumps({"transactions": count, "verified": True}), flush=True)
    finally:
        if connection:
            pq.PQfinish(connection)


if __name__ == "__main__":
    main()
