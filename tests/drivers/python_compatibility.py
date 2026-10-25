# Copyright 2026 Shujie Zhang. SPDX-License-Identifier: Apache-2.0
"""Check unmodified Psycopg and pg8000 clients against an ordinary TCP server."""

from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor
from datetime import date, datetime, timezone
from decimal import Decimal
import importlib.metadata
import json
import time
import uuid


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def check_driver(name: str, arguments: argparse.Namespace) -> dict:
    if name == "psycopg":
        import psycopg

        def connect(password: str = arguments.password):
            return psycopg.connect(
                host="127.0.0.1", port=arguments.port, dbname="postgres",
                user=arguments.user, password=password, sslmode="disable",
                connect_timeout=5, prepare_threshold=1,
            )
    else:
        import pg8000.dbapi

        def connect(password: str = arguments.password):
            return pg8000.dbapi.connect(
                host="127.0.0.1", port=arguments.port, database="postgres",
                user=arguments.user, password=password, ssl_context=False, timeout=15,
            )

    def sql(connection, text: str, parameters=None):
        cursor = connection.cursor()
        try:
            if parameters is None:
                cursor.execute(text)
            else:
                cursor.execute(text, parameters)
            return cursor.fetchall() if cursor.description else None
        finally:
            cursor.close()

    def sqlstate(error: Exception) -> str | None:
        if name == "psycopg":
            return getattr(error, "sqlstate", None)
        return error.args[0].get("C") if error.args and isinstance(error.args[0], dict) else None

    checks = []
    connection = connect()
    connection.autocommit = True
    identity = {"driver": name, "version": importlib.metadata.version(name)}
    if name == "psycopg":
        identity["libpq_version"] = psycopg.pq.version()
        identity["protocol_version"] = getattr(connection.info, "full_protocol_version", None)
    try:
        require(sql(connection, "SELECT 41+1")[0][0] == 42, "SCRAM connection failed")
        checks.append("scram_connect")
        expected = (
            9223372036854775800, Decimal("12345678901234567890.12345678"), True,
            "\u03bb ' parameter", b"\x00\x01\x7f\xff",
            uuid.UUID("00112233-4455-6677-8899-aabbccddeeff"), date(2024, 2, 29),
            datetime(2026, 10, 10, 12, 34, 56, 123456, timezone.utc), [1, None, 3], None,
        )
        query = ("SELECT %s::bigint,%s::numeric,%s::bool,%s::text,%s::bytea,"
                 "%s::uuid,%s::date,%s::timestamptz,%s::int[],NULL::text")
        for _ in range(10):
            row = tuple(sql(connection, query, expected[:-1])[0])
            require(row == expected, f"typed parameters/results changed: {row!r}")
        checks.append("typed_parameters")

        sql(connection, "CREATE TEMP TABLE py_tx(id int PRIMARY KEY)")
        connection.autocommit = False
        sql(connection, "INSERT INTO py_tx VALUES (1)")
        connection.rollback()
        require(sql(connection, "SELECT count(*) FROM py_tx")[0][0] == 0, "rollback failed")
        sql(connection, "INSERT INTO py_tx VALUES (2)")
        sql(connection, "SAVEPOINT driver_savepoint")
        try:
            sql(connection, "INSERT INTO py_tx VALUES (2)")
            raise RuntimeError("duplicate key accepted")
        except Exception as error:
            require(sqlstate(error) == "23505", f"wrong duplicate key error: {error!r}")
        sql(connection, "ROLLBACK TO driver_savepoint")
        connection.commit()
        connection.autocommit = True
        require(sql(connection, "SELECT sum(id) FROM py_tx")[0][0] == 2, "savepoint/commit failed")
        checks.append("transactions_savepoints")

        cursor = connection.cursor()
        try:
            cursor.executemany("INSERT INTO py_tx VALUES (%s)", [(i,) for i in range(3, 19)])
        finally:
            cursor.close()
        require(sql(connection, "SELECT count(*),sum(id) FROM py_tx")[0] in
                ((17, 170), [17, 170]), "batch values changed")
        checks.append("batch")

        def session(marker: int, barrier):
            other = connect()
            other.autocommit = True
            try:
                sql(other, "CREATE TEMP TABLE py_local(val int)")
                sql(other, "INSERT INTO py_local VALUES (%s)", (marker,))
                sql(other, "SELECT set_config('postgamma.driver_marker',%s,false)", (str(marker),))
                barrier.wait(timeout=10)
                pid = sql(other, "SELECT pg_backend_pid()")[0][0]
                for _ in range(25):
                    require(sql(other, "SELECT val,current_setting('postgamma.driver_marker')::int "
                                "FROM py_local")[0] in ((marker, marker), [marker, marker]),
                            "state crossed sessions")
                return pid
            finally:
                other.close()

        from threading import Barrier
        with ThreadPoolExecutor(max_workers=2) as executor:
            barrier = Barrier(2)
            futures = [executor.submit(session, i, barrier) for i in range(2)]
            pids = [future.result(timeout=20) for future in futures]
        require(len(set(pids)) == 2, "sessions shared a backend identifier")
        checks.append("concurrent_session_isolation")

        sql(connection, "SET statement_timeout='200ms'")
        try:
            sql(connection, "SELECT pg_sleep(30)")
            raise RuntimeError("statement timeout did not fire")
        except Exception as error:
            require(sqlstate(error) == "57014", f"wrong timeout error: {error!r}")
        sql(connection, "SET statement_timeout=0")
        require(sql(connection, "SELECT 42")[0][0] == 42, "connection did not recover")
        checks.append("timeout_recovery")

        if name == "psycopg":
            require(sql(connection, "SELECT count(*) FROM pg_prepared_statements")[0][0] > 0,
                    "server preparation not exercised")
            target = connect()
            target.autocommit = True
            try:
                pid = sql(target, "SELECT pg_backend_pid()")[0][0]
                def slow_query():
                    try:
                        sql(target, "SELECT pg_sleep(30)")
                        return "not-canceled"
                    except Exception as error:
                        return sqlstate(error)
                with ThreadPoolExecutor(max_workers=1) as executor:
                    future = executor.submit(slow_query)
                    deadline = time.monotonic() + 5
                    while sql(connection, "SELECT count(*) FROM pg_stat_activity "
                              "WHERE pid=%s AND wait_event='PgSleep'", (pid,))[0][0] != 1:
                        require(time.monotonic() < deadline, "cancel target did not enter its wait")
                        time.sleep(0.02)
                    target.cancel_safe(timeout=5)
                    require(future.result(timeout=5) == "57014", "CancelRequest failed")
                require(sql(target, "SELECT 42")[0][0] == 42, "canceled session did not recover")
                require(sql(connection, "SELECT 43")[0][0] == 43, "cancel crossed sessions")
            finally:
                target.close()
            checks.append("protocol_preparation_and_cancel")
    finally:
        connection.close()

    try:
        unexpected = connect("incorrect-test-password")
    except Exception as error:
        # libpq connection failures need not expose an ErrorResponse SQLSTATE.
        require(sqlstate(error) == "28P01" or (
            name == "psycopg" and isinstance(error, psycopg.OperationalError)
            and "password authentication failed" in str(error)
        ), f"wrong authentication error: {error!r}")
    else:
        unexpected.close()
        raise RuntimeError("incorrect password accepted")
    checks.append("bad_password")
    for _ in range(20):
        other = connect()
        other.autocommit = True
        try:
            require(sql(other, "SELECT 42")[0][0] == 42, "reconnected session failed")
        finally:
            other.close()
    checks.append("connection_churn")
    return {**identity, "checks": checks}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, required=True)
    parser.add_argument("--user", required=True)
    parser.add_argument("--password", required=True)
    arguments = parser.parse_args()
    reports = [check_driver(name, arguments) for name in ("psycopg", "pg8000")]
    print(json.dumps({"status": "passed", "drivers": reports}))


if __name__ == "__main__":
    main()
