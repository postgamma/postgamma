"""End-to-end executable contract for the complete Python product."""

from __future__ import annotations

import argparse
import asyncio
import datetime
import gc
import io
import json
import os
import tempfile
import threading
import time
import unittest
import uuid
import warnings
from concurrent.futures import ThreadPoolExecutor
from decimal import Decimal
from pathlib import Path

import postgamma


SCENARIO_MARKER = "POSTGAMMA_PYTHON_SCENARIOS"
REQUIRED_SCENARIOS = {
    "test_01_library_and_topology",
    "test_02_dbapi_transactions_and_errors",
    "test_03_typed_round_trip",
    "test_04_connections_execute_in_parallel_without_the_gil",
    "test_05_timeout_cancels_and_connection_is_reusable",
    "test_06_cross_thread_cancel",
    "test_07_inherited_handles_fail_closed_after_fork",
    "test_08_top_level_connect_shares_process_local_instance",
    "test_09_unclosed_handles_warn_and_release_the_cluster",
    "test_10_bounded_streaming_and_prepared_statements",
    "test_11_copy_round_trip_and_early_abort",
    "test_12_notice_notification_and_event_routing",
    "test_13_management_and_logical_round_trip",
    "test_14_arrow_capsule_protocol",
    "test_15_asyncio_waitables_parallelism_and_cancellation",
    "test_16_bundled_pgvector",
}


class PythonIntegrationTests(unittest.TestCase):
    database: postgamma.Database
    root: tempfile.TemporaryDirectory[str]

    @classmethod
    def setUpClass(cls) -> None:
        cls.root = tempfile.TemporaryDirectory(prefix="postgamma-python-test-")
        cls.database_path = Path(cls.root.name) / "database"
        if cls.database_path.exists():
            raise AssertionError(
                "lazy database path exists before Database construction"
            )
        cls.database = postgamma.Database(
            cls.database_path,
            worker_count=4,
            timeout=10,
        )
        if cls.database_path.exists():
            raise AssertionError("Database construction opened the lazy instance")
        cls.database.open()
        if not cls.database_path.is_dir():
            raise AssertionError("Database.open did not create the cluster directory")

    @classmethod
    def tearDownClass(cls) -> None:
        cls.database.close()
        cls.root.cleanup()

    def test_01_library_and_topology(self) -> None:
        info = postgamma.library_info()
        self.assertEqual(info["postgresql_version"], "19")
        self.assertGreaterEqual(info["abi_version"], (1 << 16) | 3)
        self.assertEqual(self.database.telemetry()["executor_worker_count"], 4)
        children_files = list(Path("/proc/self/task").glob("*/children"))
        if children_files:
            self.assertFalse(
                any(
                    path.read_text(encoding="ascii").strip()
                    for path in children_files
                ),
                "the embedded Python host unexpectedly has a child process",
            )
        socket_descriptors = []
        for descriptor in Path("/proc/self/fd").glob("*"):
            try:
                target = os.readlink(descriptor)
            except OSError:
                continue
            if target.startswith("socket:"):
                socket_descriptors.append((descriptor.name, target))
        self.assertEqual(socket_descriptors, [])

    def test_02_dbapi_transactions_and_errors(self) -> None:
        with self.database.connect() as connection:
            cursor = connection.cursor()
            cursor.execute(
                "create table python_accounts("
                "id bigint primary key, payload jsonb not null)"
            )
            self.assertEqual(cursor.rowcount, -1)
        connection = self.database.connect()
        cursor = connection.cursor()
        cursor.execute(
            "insert into python_accounts values (:1, :2)",
            [1, {"agent": "planner"}],
        )
        connection.rollback()
        cursor.execute("select count(*) from python_accounts")
        self.assertEqual(cursor.fetchone(), (0,))
        cursor.execute("select 'C:\\'::text, :1::bigint", [9])
        self.assertEqual(cursor.fetchone(), ("C:\\", 9))
        cursor.execute(
            "insert into python_accounts values (:1, :2)",
            [1, {"agent": "planner"}],
        )
        connection.commit()
        with self.assertRaises(postgamma.IntegrityError) as captured:
            cursor.execute(
                "insert into python_accounts values (:1, :2)",
                [1, {"agent": "duplicate"}],
            )
        self.assertEqual(captured.exception.sqlstate, "23505")
        connection.rollback()

        with connection.transaction() as transaction:
            transaction.execute(
                "insert into python_accounts values ($1, $2)",
                [2, {"transaction": "committed"}],
            )
            with self.assertRaises(postgamma.ProgrammingError):
                transaction.commit()
            with self.assertRaises(postgamma.ProgrammingError):
                with transaction.transaction():
                    pass
        self.assertEqual(
            connection.execute(
                "select payload from python_accounts where id = 2"
            ).fetchone(),
            ({"transaction": "committed"},),
        )
        connection.rollback()

        with self.assertRaisesRegex(RuntimeError, "rollback scope"):
            with connection.transaction() as transaction:
                transaction.execute(
                    "insert into python_accounts values ($1, $2)",
                    [3, {"transaction": "rolled-back"}],
                )
                raise RuntimeError("rollback scope")
        self.assertEqual(
            connection.execute(
                "select count(*) from python_accounts where id = 3"
            ).fetchone(),
            (0,),
        )
        connection.rollback()

        scope = connection.transaction()
        with scope:
            connection.execute("select 1")
        with self.assertRaises(postgamma.ProgrammingError):
            with scope:
                pass
        cursor.close()
        connection.close()

    def test_03_typed_round_trip(self) -> None:
        identifier = uuid.uuid4()
        instant = datetime.datetime(2026, 8, 28, 3, 20, 1, 123456)
        with self.database.connect(autocommit=True) as connection:
            row = connection.execute(
                "select $1::bool, $2::bigint, $3::double precision, "
                "$4::numeric, $5::text, $6::bytea, $7::date, "
                "$8::timestamp, $9::uuid, $10::jsonb, $11::bigint[], "
                "point(1, 2)",
                [
                    True,
                    42,
                    1.5,
                    Decimal("12.30"),
                    "hello",
                    b"\x00\xff",
                    datetime.date(2026, 8, 28),
                    instant,
                    identifier,
                    {"ok": True},
                    [1, 2, None],
                ],
            ).fetchone()
        self.assertEqual(
            row[:-1],
            (
                True,
                42,
                1.5,
                Decimal("12.30"),
                "hello",
                b"\x00\xff",
                datetime.date(2026, 8, 28),
                instant,
                identifier,
                {"ok": True},
                [1, 2, None],
            ),
        )
        self.assertIsInstance(row[-1], postgamma.RawValue)

    def test_04_connections_execute_in_parallel_without_the_gil(self) -> None:
        connections = [
            self.database.connect(autocommit=True),
            self.database.connect(autocommit=True),
        ]
        barrier = threading.Barrier(3)

        def query(index: int) -> int:
            barrier.wait()
            row = connections[index].execute(
                "select $1::bigint from pg_sleep(0.40)", [index]
            ).fetchone()
            return row[0]

        try:
            with ThreadPoolExecutor(max_workers=2) as executor:
                futures = [executor.submit(query, index) for index in range(2)]
                barrier.wait()
                started = time.monotonic()
                deadline = started + 0.30
                active = 0
                while time.monotonic() < deadline:
                    active = max(
                        active,
                        self.database.telemetry()["active_request_count"],
                    )
                    if active >= 2:
                        break
                    time.sleep(0.005)
                self.assertGreaterEqual(
                    active,
                    2,
                    "two Python threads never had native requests active together",
                )
                self.assertEqual([future.result() for future in futures], [0, 1])
                elapsed = time.monotonic() - started
            self.assertLess(
                elapsed,
                2.0,
                f"parallel query test stalled for {elapsed:.3f}s",
            )
        finally:
            for connection in connections:
                connection.close()

    def test_05_timeout_cancels_and_connection_is_reusable(self) -> None:
        with self.database.connect(autocommit=True) as connection:
            started = time.monotonic()
            with self.assertRaises(postgamma.QueryTimeoutError):
                connection.execute("select pg_sleep(5)", timeout=0.05)
            self.assertLess(time.monotonic() - started, 2.0)
            self.assertEqual(connection.execute("select 42").fetchone(), (42,))

    def test_06_cross_thread_cancel(self) -> None:
        connection = self.database.connect(autocommit=True)
        started = threading.Event()

        def query() -> None:
            started.set()
            connection.execute("select pg_sleep(30)")

        try:
            with ThreadPoolExecutor(max_workers=1) as executor:
                future = executor.submit(query)
                self.assertTrue(started.wait(1))
                deadline = time.monotonic() + 2
                while self.database.telemetry()["active_request_count"] == 0:
                    if time.monotonic() >= deadline:
                        self.fail("request did not become active")
                    time.sleep(0.005)
                self.assertTrue(connection.cancel())
                with self.assertRaises(postgamma.QueryCanceledError):
                    future.result(timeout=2)
            self.assertEqual(connection.execute("select 7").fetchone(), (7,))
        finally:
            connection.close()

    @unittest.skipUnless(hasattr(os, "fork"), "fork contract is POSIX-only")
    def test_07_inherited_handles_fail_closed_after_fork(self) -> None:
        connection = self.database.connect(autocommit=True)
        read_fd, write_fd = os.pipe()
        child = os.fork()
        if child == 0:
            os.close(read_fd)
            try:
                connection.execute("select 1")
            except postgamma.ForkedProcessError:
                os.write(write_fd, b"fork-rejected")
                os._exit(0)
            except BaseException:
                os._exit(2)
            os._exit(3)
        os.close(write_fd)
        message = os.read(read_fd, 64)
        os.close(read_fd)
        waited, status = os.waitpid(child, 0)
        connection.close()
        self.assertEqual(waited, child)
        self.assertTrue(os.WIFEXITED(status))
        self.assertEqual(os.WEXITSTATUS(status), 0)
        self.assertEqual(message, b"fork-rejected")

    def test_08_top_level_connect_shares_process_local_instance(self) -> None:
        path = Path(self.root.name) / "dbapi-shared"
        first = postgamma.connect(
            path,
            mode=postgamma.OpenMode.CREATE_NEW,
            autocommit=True,
            worker_count=2,
        )
        owner = first.database
        second = postgamma.connect(
            path,
            mode=postgamma.OpenMode.OPEN_EXISTING,
            autocommit=True,
            worker_count=2,
        )
        try:
            self.assertIs(second.database, owner)
            self.assertNotEqual(first.id, second.id)
            cursor = first.cursor()
            cursor.execute("select :1::bigint", [84])
            self.assertEqual(cursor.fetchone(), (84,))
            first.execute("set application_name = 'first-session'")
            self.assertEqual(
                second.execute("show application_name").fetchone(),
                ("postgamma-python",),
            )
            with self.assertRaises(postgamma.OperationalError):
                postgamma.connect(
                    path,
                    mode=postgamma.OpenMode.CREATE_NEW,
                    worker_count=2,
                )
            with self.assertRaises(postgamma.InterfaceError):
                postgamma.connect(path, worker_count=3)
        finally:
            first.close()
        self.assertFalse(owner.closed)
        self.assertEqual(second.execute("select 85").fetchone(), (85,))
        second.close()
        self.assertTrue(owner.closed)

        with self.assertRaises(postgamma.OperationalError):
            postgamma.connect(
                path,
                mode=postgamma.OpenMode.CREATE_NEW,
                worker_count=2,
            )
        reopened = postgamma.connect(
            path,
            mode=postgamma.OpenMode.OPEN_EXISTING,
            worker_count=2,
        )
        reopened.close()

        missing = Path(self.root.name) / "missing-existing"
        with self.assertRaises(postgamma.OperationalError):
            postgamma.connect(
                missing,
                mode=postgamma.OpenMode.OPEN_EXISTING,
                worker_count=2,
            )

    def test_09_unclosed_handles_warn_and_release_the_cluster(self) -> None:
        path = Path(self.root.name) / "finalizer-owned"
        database = postgamma.Database(path, worker_count=2, timeout=10)
        connection = database.connect(autocommit=True)
        connection.execute("create table finalizer_data(value bigint)")
        connection.execute("insert into finalizer_data values (91)")
        with warnings.catch_warnings(record=True) as captured:
            warnings.simplefilter("always", ResourceWarning)
            del connection
            del database
            gc.collect()
        messages = [str(item.message) for item in captured]
        self.assertTrue(any("Connection" in message for message in messages))
        self.assertTrue(any("Database" in message for message in messages))
        with postgamma.Database(
            path, mode=postgamma.OpenMode.OPEN_EXISTING, worker_count=2
        ) as reopened:
            self.assertEqual(
                reopened.execute("select value from finalizer_data").fetchone(),
                (91,),
            )

    def test_10_bounded_streaming_and_prepared_statements(self) -> None:
        with self.database.connect() as pinned:
            pinned.execute("select 1")
            self.assertTrue(
                pinned.status.pin_reasons & postgamma.PinReason.TRANSACTION
            )
            pinned.rollback()

        with self.database.connect(autocommit=True) as connection:
            connection.execute(
                "create table python_stream_probe as "
                "select i::integer as id, ('value-' || i)::text as payload "
                "from generate_series(1, 2500) i"
            )
            observed: list[tuple[object, ...]] = []
            chunk_sizes: list[int] = []
            with connection.stream(
                "select id, payload from python_stream_probe order by id",
                chunk_rows=137,
            ) as stream:
                for chunk in stream.chunks():
                    chunk_sizes.append(len(chunk.rows))
                    observed.extend(chunk.rows)
                self.assertEqual(stream.rowcount, 2500)
            self.assertEqual(len(observed), 2500)
            self.assertEqual(observed[0], (1, "value-1"))
            self.assertEqual(observed[-1], (2500, "value-2500"))
            self.assertLessEqual(max(chunk_sizes), 137)
            self.assertGreater(len(chunk_sizes), 10)

            with connection.prepare(
                "select payload from python_stream_probe where id = $1",
                parameter_type_oids=[23],
            ) as statement:
                self.assertEqual(statement.description.parameter_type_oids, (23,))
                self.assertEqual(statement.description.columns[0].name, "payload")
                self.assertEqual(statement.execute([42]).fetchone(), ("value-42",))

            pending = connection.stream("select 31", chunk_rows=1)
            with ThreadPoolExecutor(max_workers=1) as executor:
                future = executor.submit(connection.execute, "select 37")
                time.sleep(0.05)
                self.assertFalse(
                    future.done(),
                    "a second same-connection operation bypassed the request gate",
                )
                pending.close()
                self.assertEqual(future.result(timeout=2).fetchone(), (37,))

            statement = connection.prepare("select 41")
            pending = connection.stream("select 43", chunk_rows=1)
            with ThreadPoolExecutor(max_workers=1) as executor:
                future = executor.submit(statement.close)
                time.sleep(0.05)
                self.assertFalse(
                    future.done(),
                    "prepared-statement close bypassed the connection request gate",
                )
                pending.close()
                future.result(timeout=2)

            abandoned = connection.stream(
                "select i from generate_series(1, 50) i", chunk_rows=3
            )
            self.assertEqual(abandoned.fetchone(), (1,))
            with warnings.catch_warnings(record=True) as captured:
                warnings.simplefilter("always", ResourceWarning)
                del abandoned
                gc.collect()
            self.assertTrue(
                any("RowStream" in str(item.message) for item in captured)
            )
            self.assertEqual(connection.execute("select 47").fetchone(), (47,))

            delivered = 0
            with self.assertRaises(postgamma.DataError):
                with connection.stream(
                    "select 10 / (2000 - i) "
                    "from generate_series(1, 2500) i",
                    chunk_rows=100,
                ) as stream:
                    for chunk in stream.chunks():
                        delivered += len(chunk.rows)
            self.assertGreater(delivered, 0, "late stream error preceded every chunk")
            self.assertEqual(connection.execute("select 17").fetchone(), (17,))

    def test_11_copy_round_trip_and_early_abort(self) -> None:
        payload = b"1," + (b"x" * 300_000) + b"\n2,omega\n"
        with self.database.connect(autocommit=True) as connection:
            connection.execute(
                "create table python_copy_probe(id integer primary key, payload text)"
            )
            result = connection.copy_from(
                "copy python_copy_probe from stdin with (format csv)",
                io.BytesIO(payload),
                chunk_size=4093,
            )
            self.assertEqual(result.command_status, "COPY 2")
            output = io.BytesIO()
            result = connection.copy_to(
                "copy (select id, payload from python_copy_probe order by id) "
                "to stdout with (format csv)",
                output,
                chunk_size=37,
            )
            self.assertEqual(result.command_status, "COPY 2")
            self.assertEqual(output.getvalue(), payload)

            transfer = connection.copy(
                "copy python_copy_probe from stdin with (format csv)"
            )
            self.assertIsInstance(transfer, postgamma.CopyIn)
            transfer.write(b"3,not-committed\n")
            transfer.abort("intentional Python test abort")
            self.assertEqual(
                connection.execute("select count(*) from python_copy_probe").fetchone(),
                (2,),
            )

            abandoned = connection.copy(
                "copy python_copy_probe from stdin with (format csv)"
            )
            abandoned.write(b"3,abandoned\n")
            with warnings.catch_warnings(record=True) as captured:
                warnings.simplefilter("always", ResourceWarning)
                del abandoned
                gc.collect()
            self.assertTrue(
                any("CopyIn" in str(item.message) for item in captured)
            )
            self.assertEqual(
                connection.execute("select count(*) from python_copy_probe").fetchone(),
                (2,),
            )

    def test_12_notice_notification_and_event_routing(self) -> None:
        first = self.database.connect(autocommit=True)
        second = self.database.connect(autocommit=True)
        notices: list[postgamma.Event] = []
        try:
            request = postgamma.Request.start(
                first,
                "do $$ begin raise notice 'agent notice %', 7; end $$",
                timeout=10,
                on_notice=notices.append,
            )
            self.assertEqual(
                [result.command_status for result in request.results()], ["DO"]
            )
            self.assertEqual(len(notices), 1)
            self.assertIsInstance(notices[0], postgamma.NoticeEvent)
            self.assertEqual(notices[0].message, "agent notice 7")

            first.execute("listen python_agent_events")
            second.execute("notify python_agent_events, 'ready'")
            first.execute("select 1")
            events: list[postgamma.Event] = []
            while True:
                event = self.database.next_event(timeout=0)
                if event is None:
                    break
                events.append(event)
            notifications = [
                event
                for event in events
                if isinstance(event, postgamma.NotificationEvent)
            ]
            self.assertEqual(len(notifications), 1)
            self.assertEqual(notifications[0].channel, "python_agent_events")
            self.assertEqual(notifications[0].payload, "ready")
        finally:
            first.close()
            second.close()

    def test_13_management_and_logical_round_trip(self) -> None:
        self.database.execute(
            "create table python_backup_probe(id integer primary key, payload text)"
        )
        self.database.execute(
            "insert into python_backup_probe values (1, 'durable'), (2, 'portable')"
        )
        checkpoint = self.database.checkpoint(timeout=20)
        self.assertEqual(checkpoint.state, postgamma.OperationState.COMPLETED)
        maintenance = self.database.maintenance("analyze", timeout=20)
        self.assertEqual(maintenance.state, postgamma.OperationState.COMPLETED)

        archive = io.BytesIO()
        dumped = self.database.logical_dump(archive, timeout=30)
        self.assertEqual(dumped.state, postgamma.OperationState.COMPLETED)
        self.assertEqual(dumped.bytes_produced, len(archive.getvalue()))
        self.assertGreater(dumped.bytes_produced, 0)

        restored_path = Path(self.root.name) / "logical-restored"
        restored = postgamma.Database(restored_path, worker_count=2, timeout=30)
        try:
            archive.seek(0)
            progress = restored.logical_restore(archive, timeout=30)
            self.assertEqual(progress.state, postgamma.OperationState.COMPLETED)
            self.assertEqual(
                restored.execute(
                    "select id, payload from python_backup_probe order by id"
                ).fetchall(),
                [(1, "durable"), (2, "portable")],
            )
        finally:
            restored.close()
        extensions = self.database.bundled_extensions()
        self.assertTrue(
            all(extension.postgresql_major == 19 for extension in extensions)
        )

    def test_14_arrow_capsule_protocol(self) -> None:
        with self.database.connect(autocommit=True) as connection:
            exported = connection.execute_arrow(
                "select 1::int4 as id, 'agent'::text as payload, null::numeric as score"
            )
        schema, array = exported.__arrow_c_array__()
        self.assertIn('"arrow_schema"', repr(schema))
        self.assertIn('"arrow_array"', repr(array))
        self.assertTrue(exported.consumed)
        with self.assertRaises(postgamma.InterfaceError):
            exported.__arrow_c_array__()
        del schema, array
        gc.collect()

    def test_15_asyncio_waitables_parallelism_and_cancellation(self) -> None:
        async def scenario() -> None:
            implicit_path = Path(self.root.name) / "async-shared"
            implicit_first = await postgamma.connect_async(
                implicit_path,
                mode=postgamma.OpenMode.CREATE_NEW,
                worker_count=2,
            )
            implicit_second = await postgamma.connect_async(
                implicit_path,
                mode=postgamma.OpenMode.OPEN_EXISTING,
                worker_count=2,
                autocommit=True,
            )
            implicit_owner = implicit_first.sync_connection.database
            self.assertIsInstance(implicit_first, postgamma.AsyncConnection)
            self.assertIs(
                implicit_second.sync_connection.database,
                implicit_owner,
            )
            try:
                await implicit_second.execute(
                    "create table async_transactions(value integer)"
                )
                async with implicit_first.transaction() as transaction:
                    await transaction.execute(
                        "insert into async_transactions values (1)"
                    )
                    with self.assertRaises(postgamma.ProgrammingError):
                        await transaction.commit()
                    with self.assertRaises(postgamma.ProgrammingError):
                        async with transaction.transaction():
                            pass
                with self.assertRaisesRegex(RuntimeError, "async rollback"):
                    async with implicit_first.transaction() as transaction:
                        await transaction.execute(
                            "insert into async_transactions values (2)"
                        )
                        raise RuntimeError("async rollback")
                self.assertEqual(
                    (
                        await implicit_second.execute(
                            "select value from async_transactions order by value"
                        )
                    ).fetchall(),
                    [(1,)],
                )
            finally:
                await implicit_first.close()
            self.assertFalse(implicit_owner.closed)
            await implicit_second.close()
            self.assertTrue(implicit_owner.closed)

            database = postgamma.AsyncDatabase(self.database)
            first = await database.connect(autocommit=True)
            second = await database.connect(autocommit=True)
            try:
                started = time.monotonic()
                results = await asyncio.gather(
                    first.execute("select 1 from pg_sleep(0.25)"),
                    second.execute("select 2 from pg_sleep(0.25)"),
                )
                self.assertEqual(
                    [result.fetchone() for result in results], [(1,), (2,)]
                )
                self.assertLess(time.monotonic() - started, 1.5)

                for value in range(32):
                    results = await asyncio.gather(
                        first.execute("select $1::integer", [value]),
                        second.execute("select $1::integer", [-value]),
                    )
                    self.assertEqual(
                        [result.fetchone() for result in results],
                        [(value,), (-value,)],
                    )

                stream = await first.stream(
                    "select i from generate_series(1, 750) i", chunk_rows=61
                )
                rows = []
                async with stream:
                    async for row in stream:
                        rows.append(row)
                self.assertEqual((len(rows), rows[0], rows[-1]), (750, (1,), (750,)))
                self.assertEqual(stream.rowcount, 750)

                abandoned_stream = await first.stream(
                    "select i from generate_series(1, 50) i", chunk_rows=3
                )
                self.assertEqual(await abandoned_stream.fetchone(), (1,))
                with warnings.catch_warnings(record=True) as captured:
                    warnings.simplefilter("always", ResourceWarning)
                    del abandoned_stream
                    gc.collect()
                self.assertTrue(
                    any(
                        "AsyncRowStream" in str(item.message)
                        for item in captured
                    )
                )
                self.assertEqual(
                    (await first.execute("select 53")).fetchone(), (53,)
                )

                statement = await first.prepare(
                    "select $1::integer + 1", parameter_type_oids=[23]
                )
                try:
                    self.assertEqual((await statement.execute([8])).fetchone(), (9,))
                finally:
                    await statement.close()

                script = await first.execute_script("select 3; select 5")
                self.assertEqual(
                    [result.fetchone() for result in script], [(3,), (5,)]
                )

                await first.execute(
                    "create temp table python_async_copy_probe(id integer, payload text)"
                )
                copy_in = await first.copy(
                    "copy python_async_copy_probe from stdin with (format csv)"
                )
                copied = await copy_in.copy_from(
                    io.BytesIO(b"1,alpha\n2,beta\n"), chunk_size=3
                )
                self.assertEqual(copied.command_status, "COPY 2")
                output = io.BytesIO()
                copy_out = await first.copy(
                    "copy (select * from python_async_copy_probe order by id) "
                    "to stdout with (format csv)"
                )
                exported = await copy_out.copy_to(output, chunk_size=5)
                self.assertEqual(exported.command_status, "COPY 2")
                self.assertEqual(output.getvalue(), b"1,alpha\n2,beta\n")

                abandoned_copy = await first.copy(
                    "copy python_async_copy_probe from stdin with (format csv)"
                )
                await abandoned_copy.write(b"3,abandoned\n")
                with warnings.catch_warnings(record=True) as captured:
                    warnings.simplefilter("always", ResourceWarning)
                    del abandoned_copy
                    gc.collect()
                self.assertTrue(
                    any("AsyncCopy" in str(item.message) for item in captured)
                )
                self.assertEqual(
                    (
                        await first.execute(
                            "select count(*) from python_async_copy_probe"
                        )
                    ).fetchone(),
                    (2,),
                )

                await first.execute("listen python_async_events")
                await second.execute("notify python_async_events, 'async-ready'")
                await first.execute("select 1")
                event_stream = database.events()
                while True:
                    event = await asyncio.wait_for(anext(event_stream), timeout=2)
                    if isinstance(event, postgamma.NotificationEvent):
                        break
                await event_stream.aclose()
                self.assertEqual(
                    (event.channel, event.payload),
                    ("python_async_events", "async-ready"),
                )

                checkpoint = await database.checkpoint(timeout=20)
                self.assertEqual(
                    checkpoint.state, postgamma.OperationState.COMPLETED
                )

                with self.assertRaises(postgamma.QueryTimeoutError):
                    await first.execute("select pg_sleep(5)", timeout=0.05)
                self.assertEqual((await first.execute("select 23")).fetchone(), (23,))

                task = asyncio.create_task(first.execute("select pg_sleep(5)"))
                await asyncio.sleep(0.05)
                task.cancel()
                with self.assertRaises(asyncio.CancelledError):
                    await task
                self.assertEqual((await first.execute("select 29")).fetchone(), (29,))
            finally:
                await first.close()
                await second.close()

            closing_database = postgamma.AsyncDatabase(
                Path(self.root.name) / "async-events-close",
                worker_count=2,
            )
            await closing_database.open()
            while closing_database.sync_database.next_event(timeout=0) is not None:
                pass
            event_stream = closing_database.events()

            async def consume_until_close() -> None:
                async for _event in event_stream:
                    pass

            waiter = asyncio.create_task(consume_until_close())
            await asyncio.sleep(0.05)
            self.assertFalse(waiter.done())
            await closing_database.close()
            await asyncio.wait_for(waiter, timeout=2)
            await event_stream.aclose()

        asyncio.run(scenario())

    def test_16_bundled_pgvector(self) -> None:
        extensions = {
            extension.sql_name: extension
            for extension in self.database.bundled_extensions()
        }
        self.assertIn("vector", extensions)
        self.assertEqual(extensions["vector"].version, "0.8.6")
        self.assertEqual(extensions["vector"].postgresql_major, 19)
        with self.database.connect(autocommit=True) as connection:
            connection.execute("create extension if not exists vector")
            connection.execute(
                "create table python_vector_probe("
                "id integer primary key, embedding vector(3) not null)"
            )
            connection.execute(
                "insert into python_vector_probe "
                "select i, array[i::real, (i % 17)::real, "
                "(i % 31)::real]::vector "
                "from generate_series(1, 1000) as s(i)"
            )
            connection.execute(
                "create index python_vector_probe_hnsw on python_vector_probe "
                "using hnsw (embedding vector_l2_ops) "
                "with (m = 8, ef_construction = 32)"
            )
            connection.execute("set enable_seqscan = off")
            row = connection.execute(
                "select id, embedding::text from python_vector_probe "
                "order by embedding <-> $1::vector limit 1",
                ["[1,1,1]"],
            ).fetchone()
            self.assertEqual(row, (1, "[1,1,1]"))
            plan = connection.execute(
                "explain (costs off) select id from python_vector_probe "
                "order by embedding <-> '[1,1,1]' limit 1"
            ).fetchall()
            self.assertIn("python_vector_probe_hnsw", "\n".join(row[0] for row in plan))


class ScenarioResult(unittest.TextTestResult):
    def __init__(self, *args: object, **kwargs: object) -> None:
        super().__init__(*args, **kwargs)
        self.passed_scenarios: list[str] = []
        self.skipped_scenarios: list[str] = []

    @staticmethod
    def _scenario(test: unittest.case.TestCase) -> str:
        return test.id().rsplit(".", 1)[-1]

    def addSuccess(self, test: unittest.case.TestCase) -> None:
        super().addSuccess(test)
        self.passed_scenarios.append(self._scenario(test))

    def addSkip(self, test: unittest.case.TestCase, reason: str) -> None:
        super().addSkip(test, reason)
        self.skipped_scenarios.append(self._scenario(test))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--verbosity", type=int, default=2)
    args = parser.parse_args()
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(PythonIntegrationTests)
    result = unittest.TextTestRunner(
        verbosity=args.verbosity, resultclass=ScenarioResult
    ).run(suite)
    assert isinstance(result, ScenarioResult)
    document = {
        "schema_version": 1,
        "passed": sorted(result.passed_scenarios),
        "skipped": sorted(result.skipped_scenarios),
    }
    print(SCENARIO_MARKER + " " + json.dumps(document, sort_keys=True))
    complete = (
        set(result.passed_scenarios) == REQUIRED_SCENARIOS
        and len(result.passed_scenarios) == len(REQUIRED_SCENARIOS)
        and not result.skipped_scenarios
    )
    return 0 if result.wasSuccessful() and complete else 1


if __name__ == "__main__":
    raise SystemExit(main())
