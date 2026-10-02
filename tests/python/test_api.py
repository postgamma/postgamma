"""Fast unit tests for the Python-owned API and codec layer."""

from __future__ import annotations

import asyncio
import datetime
import gc
import inspect
import math
import os
import tempfile
import unittest
import uuid
import warnings
import weakref
from decimal import Decimal
from importlib.metadata import version

import postgamma
from postgamma import _async as async_module
from postgamma import _codec, _sql


class CodecTests(unittest.TestCase):
    def test_builtin_parameter_codecs_are_typed(self) -> None:
        identifier = uuid.uuid4()
        values = [
            None,
            True,
            42,
            1 << 80,
            1.5,
            Decimal("12.30"),
            "hello",
            b"bytes",
            datetime.date(2026, 8, 28),
            datetime.datetime(2026, 8, 28, 3, 0, 1),
            datetime.timedelta(days=2, seconds=1),
            identifier,
            {"ok": True},
            [1, 2, None],
        ]
        encoded = [_codec.encode_parameter(value) for value in values]
        self.assertTrue(encoded[0].is_null)
        self.assertEqual(encoded[1].oid, _codec.BOOL)
        self.assertEqual(encoded[2].oid, _codec.INT8)
        self.assertEqual(encoded[3].oid, _codec.NUMERIC)
        self.assertEqual(encoded[5].data, b"12.30")
        self.assertEqual(encoded[7].format, _codec.BINARY)
        self.assertEqual(encoded[11].data, str(identifier).encode("ascii"))
        self.assertEqual(encoded[12].oid, _codec.JSONB)
        self.assertEqual(encoded[13].oid, 1016)

    def test_unknown_oid_remains_lossless(self) -> None:
        value = _codec.decode_value(999_999, _codec.TEXT, b"extension-value")
        self.assertEqual(
            value,
            postgamma.RawValue(999_999, _codec.TEXT, b"extension-value"),
        )

    def test_custom_codec_is_reversible(self) -> None:
        class Vector:
            def __init__(self, text: str) -> None:
                self.text = text

        def encode(value: Vector) -> postgamma.EncodedParameter:
            return postgamma.EncodedParameter(42_424, 0, False, value.text.encode())

        postgamma.register_codec(
            oid=42_424,
            decoder=lambda data, _format: Vector(data.decode()),
            python_type=Vector,
            encoder=encode,
        )
        try:
            self.assertEqual(_codec.encode_parameter(Vector("[1,2]")).oid, 42_424)
            self.assertEqual(
                _codec.decode_value(42_424, 0, b"[1,2]").text,
                "[1,2]",
            )
        finally:
            postgamma.unregister_codec(42_424, Vector)


class ResultTests(unittest.TestCase):
    def test_result_has_native_and_dbapi_views(self) -> None:
        result = postgamma.Result(
            {
                "kind": 1,
                "command_status": "SELECT 2",
                "columns": (("value", 0, 0, _codec.INT8, 8, -1, 0),),
                "rows": (((_codec.INT8, 0, b"1"),), ((_codec.INT8, 0, b"2"),)),
            }
        )
        self.assertEqual(result.rowcount, 2)
        self.assertEqual(result.description[0][0], "value")
        self.assertEqual(result.fetchone(), (1,))
        self.assertEqual(result.fetchall(), [(2,)])


class ModuleContractTests(unittest.TestCase):
    def test_pep249_metadata(self) -> None:
        self.assertEqual(postgamma.apilevel, "2.0")
        self.assertEqual(postgamma.threadsafety, 2)
        self.assertEqual(postgamma.paramstyle, "numeric")
        self.assertEqual(postgamma.__version__, "0.1.0a1")
        self.assertEqual(version("postgamma"), postgamma.__version__)
        self.assertEqual(postgamma.library_info()["postgresql_version"], "19")

    def test_required_dbapi_constructors_and_type_objects(self) -> None:
        self.assertEqual(postgamma.Date(2026, 8, 28), datetime.date(2026, 8, 28))
        self.assertEqual(postgamma.Time(3, 4, 5), datetime.time(3, 4, 5))
        self.assertEqual(
            postgamma.Timestamp(2026, 8, 28, 3, 4, 5),
            datetime.datetime(2026, 8, 28, 3, 4, 5),
        )
        self.assertEqual(postgamma.Binary(bytearray(b"value")), b"value")
        self.assertEqual(_codec.TEXT_OID, postgamma.STRING)
        self.assertEqual(_codec.INT8, postgamma.NUMBER)
        self.assertNotEqual(_codec.JSONB, postgamma.NUMBER)

    def test_numeric_parameter_markers_are_sql_aware(self) -> None:
        operation = (
            "select :1, ':2', \"column:3\", value::int, $body$:4$body$ "
            "-- :5\n/* :6 /* :7 */ */ , :2"
        )
        self.assertEqual(
            _sql.rewrite_numeric_parameters(operation),
            "select $1, ':2', \"column:3\", value::int, $body$:4$body$ "
            "-- :5\n/* :6 /* :7 */ */ , $2",
        )
        with self.assertRaises(ValueError):
            _sql.rewrite_numeric_parameters("select :0")

    def test_numeric_markers_follow_standard_conforming_strings(self) -> None:
        self.assertEqual(
            _sql.rewrite_numeric_parameters("select 'C:\\' as path, :1"),
            "select 'C:\\' as path, $1",
        )
        self.assertEqual(
            _sql.rewrite_numeric_parameters(r"select E'a\'b', :1"),
            r"select E'a\'b', $1",
        )
        self.assertEqual(
            _sql.rewrite_numeric_parameters("select someE'C:\\', :1"),
            "select someE'C:\\', $1",
        )

    def test_transaction_control_uses_the_sql_lexer(self) -> None:
        self.assertTrue(
            _sql.is_transaction_control(
                "/* outer /* nested */ comment */ START /* split */ TRANSACTION"
            )
        )
        self.assertTrue(_sql.is_transaction_control("-- lead\nPREPARE TRANSACTION 'x'"))
        self.assertFalse(
            _sql.is_transaction_control("/* outer /* nested */ comment */ SELECT 1")
        )

    def test_native_status_constants_come_from_the_c_abi(self) -> None:
        from postgamma import _native

        for name in (
            "PGM_STATUS_INVALID_ARGUMENT",
            "PGM_STATUS_TIMEOUT",
            "PGM_STATUS_CANCELED",
            "PGM_STATUS_FORKED_PROCESS",
            "PGM_STATUS_UNSUPPORTED",
            "PGM_STATUS_INTERNAL_ERROR",
        ):
            self.assertIsInstance(getattr(_native, name), int)

    def test_context_cleanup_does_not_mask_a_body_exception(self) -> None:
        class BrokenDatabase(postgamma.Database):
            def __init__(self) -> None:
                pass

            def close(self, **_kwargs: object) -> None:
                raise RuntimeError("cleanup failed")

        primary = ValueError("body failed")
        with warnings.catch_warnings(record=True) as captured:
            warnings.simplefilter("always", ResourceWarning)
            BrokenDatabase().__exit__(ValueError, primary, None)
        self.assertEqual(len(captured), 1)
        self.assertIn("body failed", repr(primary))

    def test_connection_cleanup_preserves_primary_error(self) -> None:
        class BrokenConnection(postgamma.Connection):
            def __init__(self) -> None:
                pass

            def rollback(self) -> None:
                raise RuntimeError("rollback failed")

            def close(self, **_kwargs: object) -> None:
                raise RuntimeError("close failed")

        primary = ValueError("body failed")
        with warnings.catch_warnings(record=True) as captured:
            warnings.simplefilter("always", ResourceWarning)
            BrokenConnection().__exit__(ValueError, primary, None)
        self.assertEqual(len(captured), 2)

    def test_invalid_timeouts_fail_before_open(self) -> None:
        with tempfile.TemporaryDirectory() as root:
            for value in (-1, math.inf, math.nan):
                with self.subTest(value=value):
                    with self.assertRaises(ValueError):
                        postgamma.Database(root, timeout=value)

    def test_complete_python_surface_is_exported(self) -> None:
        required = {
            "ArrowResult",
            "AsyncConnection",
            "AsyncCopy",
            "AsyncDatabase",
            "AsyncRowStream",
            "Capability",
            "CopyIn",
            "CopyOut",
            "Event",
            "LogicalFlags",
            "Operation",
            "OpenMode",
            "PreparedStatement",
            "Request",
            "RowStream",
            "connect_async",
        }
        self.assertEqual(len(postgamma.__all__), len(set(postgamma.__all__)))
        self.assertLessEqual(required, set(postgamma.__all__))
        capabilities = postgamma.capabilities()
        for capability in (
            postgamma.Capability.PREPARED_STATEMENTS,
            postgamma.Capability.CHUNKED_RESULTS,
            postgamma.Capability.COPY_IN,
            postgamma.Capability.COPY_OUT,
            postgamma.Capability.ARROW_C_DATA,
            postgamma.Capability.INSTANCE_EVENTS,
            postgamma.Capability.MANAGEMENT_OPERATIONS,
            postgamma.Capability.LOGICAL_BACKUP,
            postgamma.Capability.LOGICAL_RESTORE,
        ):
            self.assertTrue(capabilities & capability)

    def test_arrow_adapter_is_one_shot_and_dependency_free(self) -> None:
        schema = object()
        array = object()
        result = postgamma.ArrowResult((schema, array))
        self.assertEqual(result.__arrow_c_array__(), (schema, array))
        self.assertTrue(result.consumed)
        with self.assertRaises(postgamma.InterfaceError):
            result.__arrow_c_array__()
        with self.assertRaises(postgamma.NotSupportedError):
            postgamma.ArrowResult((schema, array)).__arrow_c_array__(object())
        self.assertFalse(hasattr(postgamma.ArrowResult, "__arrow_c_stream__"))

    def test_connection_entry_points_are_symmetric(self) -> None:
        self.assertTrue(inspect.iscoroutinefunction(postgamma.connect_async))
        self.assertFalse(hasattr(postgamma, "async_connect"))
        sync_parameters = inspect.signature(postgamma.connect).parameters
        async_parameters = inspect.signature(postgamma.connect_async).parameters
        self.assertEqual(tuple(sync_parameters), tuple(async_parameters))
        self.assertEqual(postgamma.OpenMode.OPEN_OR_CREATE, "open_or_create")
        self.assertEqual(
            postgamma.OpenMode.parse("open_existing"),
            postgamma.OpenMode.OPEN_EXISTING,
        )
        with self.assertRaises(ValueError):
            postgamma.OpenMode.parse("truncate")

    def test_async_database_construction_remains_lazy(self) -> None:
        with tempfile.TemporaryDirectory() as root:
            path = root + "/database"
            database = postgamma.AsyncDatabase(path)
            self.assertIsInstance(database, postgamma.AsyncDatabase)
            self.assertFalse(database.sync_database.opened)
            self.assertFalse(database.sync_database.closed)

    def test_async_fd_hubs_do_not_retain_closed_event_loops(self) -> None:
        loop_references: list[
            weakref.ReferenceType[asyncio.AbstractEventLoop]
        ] = []

        for _ in range(3):
            read_descriptor, write_descriptor = os.pipe()

            async def wait_once() -> None:
                loop_references.append(weakref.ref(asyncio.get_running_loop()))
                with self.assertRaises(asyncio.TimeoutError):
                    await async_module._hub(read_descriptor).wait(0.001)

            try:
                asyncio.run(wait_once())
            finally:
                os.close(read_descriptor)
                os.close(write_descriptor)

        gc.collect()
        self.assertEqual(len(async_module._LOOP_HUBS), 0)
        self.assertTrue(all(reference() is None for reference in loop_references))

    def test_management_enums_derive_from_native_constants(self) -> None:
        from postgamma import _native

        self.assertEqual(
            postgamma.MaintenanceKind.VACUUM_ANALYZE,
            _native.MAINTENANCE_VACUUM_ANALYZE,
        )
        self.assertEqual(
            postgamma.OperationState.COMPLETED,
            _native.OPERATION_COMPLETED,
        )
        self.assertEqual(postgamma.PinReason.COPY, _native.PIN_COPY)


if __name__ == "__main__":
    unittest.main()
