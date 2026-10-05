"""Exercise Python close recovery with a controlled native API boundary."""

from __future__ import annotations

import importlib
import math
import sys
import types
import unittest
from pathlib import Path
from unittest import mock


PROJECT_ROOT = Path(__file__).resolve().parents[2]


class NativeFailure(Exception):
    def __init__(self, message: str, status: int) -> None:
        super().__init__(message)
        self.status = status


class ConnectionCloseTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        # Import the actual Python implementation without a compiled kernel.
        # The private package name avoids replacing an installed postgamma.
        name = "_postgamma_close_test"
        package = types.ModuleType(name)
        package.__path__ = [str(PROJECT_ROOT / "python/src/postgamma")]
        cls.native = types.ModuleType(f"{name}._native")
        cls.native.NativeError = NativeFailure
        cls.native.PGM_STATUS_FORKED_PROCESS = 1
        cls.native.PGM_STATUS_UNSUPPORTED = 2
        cls.native.PGM_STATUS_TIMEOUT = 3
        cls.native.TRANSACTION_IDLE = 0
        cls.native.TRANSACTION_INTRANS = 2
        cls.native.SHUTDOWN_SMART = 0
        cls.native.SHUTDOWN_FAST = 1
        cls.native.SHUTDOWN_IMMEDIATE = 2
        with mock.patch.dict(
            sys.modules, {name: package, f"{name}._native": cls.native}
        ), mock.patch("os.register_at_fork", create=True):
            cls.api = importlib.import_module(f"{name}._api")
            cls.errors = importlib.import_module(f"{name}._errors")

    def setUp(self) -> None:
        self.native.connection_transaction_status = mock.Mock(
            return_value=self.native.TRANSACTION_IDLE
        )
        self.native.connection_close = mock.Mock()
        self.native.connection_execute = mock.Mock(
            side_effect=AssertionError("SQL reached the native API during close")
        )
        self.native.connection_cancel = mock.Mock(return_value=False)
        self.native.instance_close = mock.Mock()
        self.database = self.api.Database("unused-close-test-path")
        self.database._handle = object()
        self.handle = object()
        self.release = mock.Mock()
        self.connection = self.api.Connection(
            self.database,
            self.handle,
            autocommit=True,
            timeout=30,
            implicit_release=self.release,
        )
        self.database._connections.add(self.connection)

    def tearDown(self) -> None:
        # These are test-owned sentinels, not native resources to finalize.
        self.connection._handle = None
        self.database._handle = None

    def timeout_error(self) -> NativeFailure:
        return NativeFailure(
            "backend release timed out", self.native.PGM_STATUS_TIMEOUT
        )

    def test_partial_close_retries_without_touching_the_released_protocol(self) -> None:
        request = mock.Mock()
        self.connection._requests.add(request)
        self.native.connection_close.side_effect = [
            self.timeout_error(),
            self.timeout_error(),
            None,
        ]
        with self.assertRaises(self.errors.QueryTimeoutError):
            self.connection.close(timeout=0)
        self.assertFalse(self.connection.closed)
        self.assertIs(self.connection._handle, self.handle)
        self.release.assert_not_called()
        self.native.connection_transaction_status.side_effect = AssertionError(
            "the native protocol connection has already been released"
        )
        with self.assertRaises(self.errors.QueryTimeoutError):
            self.connection.close(timeout=0)
        self.connection.close(timeout=10)
        self.connection.close(timeout=10)
        self.assertTrue(self.connection.closed)
        self.assertIsNone(self.connection._handle)
        self.assertEqual(self.native.connection_close.call_count, 3)
        self.native.connection_close.assert_called_with(
            self.handle, timeout_ms=10000
        )
        self.native.connection_transaction_status.assert_called_once_with(self.handle)
        request.close.assert_called_once()
        self.release.assert_called_once_with(10)

    def test_pending_close_rejects_queries_and_transaction_operations(self) -> None:
        self.native.connection_close.side_effect = self.timeout_error()
        with self.assertRaises(self.errors.QueryTimeoutError):
            self.connection.close(timeout=0)
        operations = (
            lambda: self.connection.execute("SELECT 1"),
            lambda: self.connection.transaction_status,
            self.connection.commit,
            self.connection.rollback,
            self.connection.cursor,
            self.connection.cancel,
        )
        for operation in operations:
            with self.subTest(operation=operation):
                with self.assertRaisesRegex(self.api.InterfaceError, "close.*pending"):
                    operation()
        self.native.connection_transaction_status.assert_called_once_with(self.handle)
        self.native.connection_execute.assert_not_called()
        self.native.connection_cancel.assert_not_called()

    def test_database_close_retries_its_pending_connection(self) -> None:
        self.connection._implicit_release = None
        self.native.connection_close.side_effect = [self.timeout_error(), None]
        with self.assertRaises(self.errors.QueryTimeoutError):
            self.database.close(timeout=0)
        self.assertFalse(self.database.closed)
        self.native.instance_close.assert_not_called()
        self.native.connection_transaction_status.side_effect = AssertionError(
            "the native protocol connection has already been released"
        )
        self.database.close(timeout=10)
        self.database.close(timeout=10)
        self.assertTrue(self.connection.closed)
        self.assertTrue(self.database.closed)
        self.assertEqual(self.native.connection_close.call_count, 2)
        self.native.instance_close.assert_called_once()

    def test_active_transaction_rolls_back_once_before_native_close(self) -> None:
        self.native.connection_transaction_status.return_value = (
            self.native.TRANSACTION_INTRANS
        )
        with mock.patch.object(self.connection, "_native_execute") as execute:
            self.native.connection_close.side_effect = [self.timeout_error(), None]
            with self.assertRaises(self.errors.QueryTimeoutError):
                self.connection.close(timeout=0)
            self.connection.close(timeout=10)
            execute.assert_called_once_with("ROLLBACK", None, self.connection.timeout)
        self.assertTrue(self.connection.closed)

    def test_rollback_failure_does_not_start_native_close(self) -> None:
        self.native.connection_transaction_status.return_value = (
            self.native.TRANSACTION_INTRANS
        )
        with mock.patch.object(
            self.connection,
            "_native_execute",
            side_effect=[RuntimeError("rollback failed"), None],
        ) as execute:
            with self.assertRaisesRegex(RuntimeError, "rollback failed"):
                self.connection.close()
            self.native.connection_close.assert_not_called()
            self.connection._require_open()
            self.connection.close()
            self.assertEqual(execute.call_count, 2)
        self.assertTrue(self.connection.closed)

    def test_child_cleanup_failure_does_not_start_native_close(self) -> None:
        cursor = self.connection.cursor()
        with mock.patch.object(
            cursor, "close", side_effect=RuntimeError("cursor failed")
        ):
            with self.assertRaisesRegex(RuntimeError, "cursor failed"):
                self.connection.close()
        self.connection._require_open()
        self.native.connection_close.assert_not_called()
        self.native.connection_transaction_status.assert_not_called()
        self.connection.close()
        self.assertTrue(cursor.closed)
        self.assertTrue(self.connection.closed)

    def test_invalid_timeout_does_not_close_children_or_start_shutdown(self) -> None:
        cursor = self.connection.cursor()
        for timeout in (-1, math.inf, math.nan, 1e20):
            with self.subTest(timeout=timeout):
                with self.assertRaises((ValueError, OverflowError)):
                    self.connection.close(timeout=timeout)
                self.assertFalse(cursor.closed)
                self.connection._require_open()
        self.native.connection_close.assert_not_called()
        self.native.connection_transaction_status.assert_not_called()

    def test_normal_close_releases_children_and_owner_once(self) -> None:
        cursor = self.connection.cursor()
        self.connection.close()
        self.connection.close()
        self.assertTrue(cursor.closed)
        self.assertTrue(self.connection.closed)
        self.native.connection_close.assert_called_once_with(
            self.handle, timeout_ms=30000
        )
        self.release.assert_called_once_with(None)


if __name__ == "__main__":
    unittest.main()
