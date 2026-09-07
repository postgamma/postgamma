"""Caller-driven requests, bounded rows, COPY, and prepared statements."""

from __future__ import annotations

import select
import time
import weakref
from collections.abc import Iterator, Sequence
from dataclasses import dataclass
from typing import Any

from . import _native
from ._api import (
    Column,
    Connection,
    Result,
    _nonnegative_int,
    _normalize_timeout,
    _positive_int,
    _cleanup_preserving,
    _timeout_milliseconds,
    _warn_preserved_cleanup_error,
    _warn_resource_leak,
)
from ._codec import encode_parameters
from ._capabilities import PinReason
from ._errors import (
    InterfaceError,
    ProgrammingError,
    QueryTimeoutError,
    raise_translated,
)


_POLL_SECONDS = 0.05


@dataclass(frozen=True, slots=True)
class ConnectionStatus:
    """A synchronized transaction and executor-scheduling snapshot."""

    transaction_status: int
    pin_reasons: PinReason
    carrier_retained: bool
    virtual_backend_pid: int
    connection_id: int
    active_request_id: int

    @classmethod
    def from_native(cls, value: dict[str, Any]) -> ConnectionStatus:
        """Construct a typed status snapshot from the native ABI mapping."""

        return cls(
            transaction_status=int(value["transaction_status"]),
            pin_reasons=PinReason(int(value["pin_reasons"])),
            carrier_retained=bool(value["carrier_retained"]),
            virtual_backend_pid=int(value["virtual_backend_pid"]),
            connection_id=int(value["connection_id"]),
            active_request_id=int(value["active_request_id"]),
        )

    @property
    def pinned(self) -> bool:
        """Return whether this session currently retains a carrier worker."""

        return self.pin_reasons != 0


@dataclass(frozen=True, slots=True)
class StatementDescription:
    """Prepared-statement parameter OIDs and result-column metadata."""

    parameter_type_oids: tuple[int, ...]
    columns: tuple[Column, ...]


class Request:
    """One ordered, caller-driven PostgreSQL result sequence."""

    __slots__ = (
        "connection",
        "_handle",
        "_closed",
        "_deadline",
        "_on_notice",
        "_copy_ref",
        "_owns_gate",
        "id",
        "waitable",
        "__weakref__",
    )

    def __init__(
        self,
        connection: Connection,
        handle: Any,
        *,
        timeout: float | None,
        on_notice: Any | None = None,
    ) -> None:
        self.connection = connection
        self._handle = handle
        self._closed = False
        normalized = _normalize_timeout(timeout)
        self._deadline = (
            None if normalized is None else time.monotonic() + normalized
        )
        if on_notice is not None and not callable(on_notice):
            raise TypeError("notice handler must be callable")
        self._on_notice = on_notice
        self._copy_ref: weakref.ReferenceType[CopyIn | CopyOut] | None = None
        self._owns_gate = True
        try:
            self.id = int(_native.request_identity(handle))
            self.waitable = int(_native.request_waitable(handle))
        except _native.NativeError as error:
            try:
                _native.request_close(handle)
            finally:
                self._handle = None
                self._closed = True
                self._owns_gate = False
                connection._request_gate.release()
                raise_translated(error)
        connection._requests.add(self)
        if on_notice is not None:
            connection.database._request_event_handlers.setdefault(self.id, []).append(
                on_notice
            )

    @classmethod
    def start(
        cls,
        connection: Connection,
        sql: str,
        parameters: Sequence[Any] | None = None,
        *,
        chunk_rows: int = 0,
        timeout: float | None = None,
        result_buffer_limit: int = 0,
        script: bool = False,
        on_notice: Any | None = None,
    ) -> Request:
        """Submit SQL and return one caller-driven ordered request."""

        chunk_rows = _nonnegative_int(chunk_rows, "chunk_rows")
        result_buffer_limit = _nonnegative_int(
            result_buffer_limit, "result_buffer_limit"
        )
        if on_notice is not None and not callable(on_notice):
            raise TypeError("notice handler must be callable")
        encoded = encode_parameters(parameters)
        connection._request_gate.acquire()
        try:
            handle = _native.request_start(
                connection._handle,
                sql,
                encoded,
                chunk_rows=chunk_rows,
                result_format=_native.FORMAT_TEXT,
                result_buffer_limit=result_buffer_limit,
                script=script,
            )
        except _native.NativeError as error:
            connection._request_gate.release()
            raise_translated(error)
        except BaseException:
            connection._request_gate.release()
            raise
        return cls(connection, handle, timeout=timeout, on_notice=on_notice)

    @classmethod
    def from_statement(
        cls,
        statement: PreparedStatement,
        parameters: Sequence[Any] | None,
        *,
        chunk_rows: int,
        timeout: float | None,
        result_buffer_limit: int,
        on_notice: Any | None,
    ) -> Request:
        """Execute a prepared statement as a caller-driven request."""

        if on_notice is not None and not callable(on_notice):
            raise TypeError("notice handler must be callable")
        encoded = encode_parameters(parameters)
        statement.connection._request_gate.acquire()
        try:
            handle = _native.statement_execute(
                statement._handle,
                encoded,
                chunk_rows=chunk_rows,
                result_format=_native.FORMAT_TEXT,
                result_buffer_limit=result_buffer_limit,
            )
        except _native.NativeError as error:
            statement.connection._request_gate.release()
            raise_translated(error)
        except BaseException:
            statement.connection._request_gate.release()
            raise
        return cls(statement.connection, handle, timeout=timeout, on_notice=on_notice)

    @property
    def closed(self) -> bool:
        """Return whether this request has released its native handle."""

        return self._closed

    @property
    def state(self) -> int:
        """Inspect the request state without driving progress."""

        self._require_open()
        try:
            return int(_native.request_poll(self._handle))
        except _native.NativeError as error:
            raise_translated(error)

    def _require_open(self) -> None:
        if self._closed or self._handle is None:
            raise InterfaceError("PostGamma request is closed")

    def _remaining(self) -> float | None:
        if self._deadline is None:
            return None
        return max(0.0, self._deadline - time.monotonic())

    def _timed_out(self) -> None:
        timeout_error = QueryTimeoutError(
            "PostGamma request exceeded its host-side deadline",
            status=_native.PGM_STATUS_TIMEOUT,
            status_name="timeout",
        )
        try:
            self.cancel()
        except BaseException as cleanup_error:
            _warn_preserved_cleanup_error(
                "timed-out request cancellation", cleanup_error, timeout_error
            )
        _cleanup_preserving("timed-out request close", self.close, timeout_error)
        raise timeout_error

    def _take(self, *, block: bool = True) -> tuple[int, Any | None]:
        self._require_open()
        while True:
            remaining = self._remaining()
            if block and remaining is not None and remaining <= 0:
                self._timed_out()
            wait_seconds = 0.0 if not block else _POLL_SECONDS
            if remaining is not None:
                wait_seconds = min(wait_seconds, remaining)
            try:
                availability, native = _native.request_next(
                    self._handle,
                    timeout_ms=_timeout_milliseconds(wait_seconds),
                )
            except _native.NativeError as error:
                try:
                    raise_translated(error)
                except BaseException as primary_error:
                    _cleanup_preserving(
                        "failed request close", self.close, primary_error
                    )
                    raise
            except BaseException as primary_error:
                _cleanup_preserving(
                    "failed request close", self.close, primary_error
                )
                raise
            try:
                self.connection.database.dispatch_events()
            except BaseException as primary_error:
                _cleanup_preserving(
                    "event-handler request close", self.close, primary_error
                )
                raise
            availability = int(availability)
            if availability != _native.AVAILABILITY_AGAIN or not block:
                return availability, native

    def try_next(self) -> tuple[int, Result | CopyIn | CopyOut | None]:
        """Take one result without blocking and expose its availability state."""

        availability, native = self._take(block=False)
        if availability != _native.AVAILABILITY_READY:
            if availability == _native.AVAILABILITY_END:
                self.close()
            return availability, None
        assert native is not None
        return availability, self._convert(native)

    def next_result(self) -> Result | CopyIn | CopyOut | None:
        """Block for one ordered result, returning ``None`` at end of sequence."""

        availability, native = self._take(block=True)
        if availability == _native.AVAILABILITY_END:
            self.close()
            return None
        if availability != _native.AVAILABILITY_READY or native is None:
            raise InterfaceError("PostGamma returned an invalid result availability")
        return self._convert(native)

    def _convert(self, native: dict[str, Any]) -> Result | CopyIn | CopyOut:
        kind = int(native["kind"])
        copy_handle = native.get("copy")
        if kind == _native.RESULT_COPY_IN and copy_handle is not None:
            copy = CopyIn(self, copy_handle)
            self._copy_ref = weakref.ref(copy)
            return copy
        if kind == _native.RESULT_COPY_OUT and copy_handle is not None:
            copy = CopyOut(self, copy_handle)
            self._copy_ref = weakref.ref(copy)
            return copy
        return Result(native)

    def results(self) -> Iterator[Result]:
        """Yield all ordinary results and verify the terminal boundary."""

        while True:
            result = self.next_result()
            if result is None:
                return
            if not isinstance(result, Result):
                result.close()
                raise ProgrammingError(
                    "COPY result requires Connection.copy(), copy_from(), or copy_to()"
                )
            yield result

    def progress(self) -> int:
        """Perform one nonblocking progress step and dispatch ready events."""

        self._require_open()
        try:
            state = int(_native.request_progress(self._handle))
        except _native.NativeError as error:
            raise_translated(error)
        self.connection.database.dispatch_events()
        return state

    def cancel(self) -> bool:
        """Request PostgreSQL cancellation and report whether it was routed."""

        if self._closed or self._handle is None:
            return False
        try:
            return bool(_native.request_cancel(self._handle))
        except _native.NativeError as error:
            raise_translated(error)

    def close(self) -> None:
        """Retire this request and release the connection execution lease."""

        if self._closed:
            return
        copy = self._copy_ref() if self._copy_ref is not None else None
        if copy is not None and not copy.closed:
            copy.close()
            if self._closed:
                return
        handle = self._handle
        try:
            if handle is not None:
                _native.request_close(handle)
        except _native.NativeError as error:
            raise_translated(error)
        self._handle = None
        self._closed = True
        self.connection._requests.discard(self)
        if self._on_notice is not None:
            handlers = self.connection.database._request_event_handlers.get(self.id, [])
            try:
                handlers.remove(self._on_notice)
            except ValueError:
                pass
            if not handlers:
                self.connection.database._request_event_handlers.pop(self.id, None)
        if self._owns_gate:
            self._owns_gate = False
            self.connection._request_gate.release()

    def __enter__(self) -> Request:
        self._require_open()
        return self

    def __exit__(self, exc_type: Any, exc: Any, traceback: Any) -> None:
        del exc_type, exc, traceback
        self.close()

    def __del__(self) -> None:
        if getattr(self, "_handle", None) is not None and not getattr(
            self, "_closed", True
        ):
            _warn_resource_leak("unclosed PostGamma Request", self)
            try:
                self.close()
            except BaseException as cleanup_error:
                _warn_preserved_cleanup_error(
                    "abandoned request close", cleanup_error, None
                )


class RowStream:
    """A bounded synchronous row stream with terminal-result verification."""

    __slots__ = (
        "request",
        "columns",
        "command_status",
        "rowcount",
        "_rows",
        "_chunk_iterator",
        "_rows_seen",
        "_closed",
        "__weakref__",
    )

    def __init__(self, request: Request) -> None:
        self.request = request
        self.columns: tuple[Column, ...] = ()
        self.command_status: str | None = None
        self.rowcount = -1
        self._rows: Iterator[tuple[Any, ...]] = iter(())
        self._chunk_iterator: Iterator[Result] = self.chunks()
        self._rows_seen = 0
        self._closed = False

    @classmethod
    def start(
        cls,
        connection: Connection,
        sql: str,
        parameters: Sequence[Any] | None,
        *,
        chunk_rows: int,
        timeout: float | None,
        result_buffer_limit: int,
        on_notice: Any | None,
    ) -> RowStream:
        """Submit a bounded-row request and return its stream."""

        _positive_int(chunk_rows, "chunk_rows")
        request = Request.start(
            connection,
            sql,
            parameters,
            chunk_rows=chunk_rows,
            timeout=timeout,
            result_buffer_limit=result_buffer_limit,
            on_notice=on_notice,
        )
        return cls(request)

    @classmethod
    def from_request(cls, request: Request) -> RowStream:
        """Wrap an existing chunked request as a row stream."""

        return cls(request)

    @property
    def closed(self) -> bool:
        """Return whether this stream reached its terminal boundary or closed."""

        return self._closed

    @property
    def description(self) -> tuple[tuple[Any, ...], ...] | None:
        """Return PEP 249 descriptions after column metadata becomes available."""

        return (
            tuple(column.dbapi_description for column in self.columns)
            if self.columns
            else None
        )

    def chunks(self) -> Iterator[Result]:
        """Yield bounded result chunks and consume the terminal result."""

        if self._closed:
            raise InterfaceError("PostGamma row stream is closed")
        try:
            while True:
                result = self.request.next_result()
                if result is None:
                    self._closed = True
                    return
                if not isinstance(result, Result):
                    result.close()
                    raise ProgrammingError("row stream received a COPY result")
                if result.columns and not self.columns:
                    self.columns = result.columns
                if result.kind == _native.RESULT_TUPLES_CHUNK:
                    self._rows_seen += len(result.rows)
                    yield result
                    continue
                self.command_status = result.command_status
                self._rows_seen += len(result.rows)
                self.rowcount = self._rows_seen
                if result.rows:
                    yield result
        except BaseException as primary_error:
            _cleanup_preserving("row-stream close", self.close, primary_error)
            raise

    def fetchone(self) -> tuple[Any, ...] | None:
        """Return the next streamed row, or ``None`` at the terminal boundary."""

        while True:
            try:
                return next(self._rows)
            except StopIteration:
                try:
                    chunk = next(self._chunk_iterator)
                except StopIteration:
                    return None
                self._rows = iter(chunk.rows)

    def fetchmany(self, size: int = 1) -> list[tuple[Any, ...]]:
        """Return up to ``size`` rows while preserving bounded native chunks."""

        size = _nonnegative_int(size, "size")
        rows: list[tuple[Any, ...]] = []
        while len(rows) < size:
            row = self.fetchone()
            if row is None:
                break
            rows.append(row)
        return rows

    def fetchall(self) -> list[tuple[Any, ...]]:
        """Materialize and return every remaining streamed row in Python."""

        return list(self)

    def close(self) -> None:
        """Retire the active request and close this stream."""

        if self._closed:
            return
        self.request.close()
        self._closed = True
        self._rows = iter(())
        self._chunk_iterator = iter(())

    def __iter__(self) -> RowStream:
        return self

    def __next__(self) -> tuple[Any, ...]:
        row = self.fetchone()
        if row is None:
            raise StopIteration
        return row

    def __enter__(self) -> RowStream:
        if self._closed:
            raise InterfaceError("PostGamma row stream is closed")
        return self

    def __exit__(self, exc_type: Any, exc: Any, traceback: Any) -> None:
        del exc_type, exc, traceback
        self.close()

    def __del__(self) -> None:
        if getattr(self, "_closed", True):
            return
        _warn_resource_leak("unclosed PostGamma RowStream", self)
        try:
            self.close()
        except BaseException as cleanup_error:
            _warn_preserved_cleanup_error(
                "abandoned row-stream close", cleanup_error, None
            )


class _CopyBase:
    __slots__ = (
        "request",
        "_handle",
        "_closed",
        "_io_ended",
        "command_result",
        "__weakref__",
    )

    def __init__(self, request: Request, handle: Any) -> None:
        self.request = request
        self._handle = handle
        self._closed = False
        self._io_ended = False
        self.command_result: Result | None = None

    @property
    def closed(self) -> bool:
        """Return whether the COPY handle and its request are closed."""

        return self._closed

    def _require_open(self) -> None:
        if self._closed or self._handle is None:
            raise InterfaceError("PostGamma COPY stream is closed")

    def _progress_wait(self) -> None:
        self.request.progress()
        remaining = self.request._remaining()
        if remaining is not None and remaining <= 0:
            self.request._timed_out()
        wait = _POLL_SECONDS if remaining is None else min(_POLL_SECONDS, remaining)
        select.select([self.request.waitable], [], [], wait)

    def _release_copy(self) -> None:
        self._require_open()
        try:
            _native.copy_close(
                self._handle,
                timeout_ms=_timeout_milliseconds(self.request._remaining()),
            )
        except _native.NativeError as error:
            raise_translated(error)
        self._handle = None
        self._closed = True

    def _finish_request(self) -> Result:
        results = list(self.request.results())
        self.request.close()
        if len(results) != 1:
            raise InterfaceError("COPY did not produce exactly one command result")
        self.command_result = results[0]
        return results[0]

    def abort(self, message: str | None = None) -> None:
        """Abort COPY and release the active request.

        Args:
            message: Optional PostgreSQL COPY failure message.
        """

        if self._closed:
            return
        primary_error: BaseException | None = None
        try:
            _native.copy_abort(self._handle, message=message)
            self._release_copy()
        except _native.NativeError as error:
            primary_error = error
            raise_translated(error)
        except BaseException as error:
            primary_error = error
            raise
        finally:
            _cleanup_preserving(
                "aborted COPY request close", self.request.close, primary_error
            )

    def close(self) -> None:
        """Close COPY and release its request without completing input."""

        if self._closed:
            if not self.request.closed:
                self.request.close()
            return
        primary_error: BaseException | None = None
        try:
            self._release_copy()
        except BaseException as error:
            primary_error = error
            raise
        finally:
            _cleanup_preserving("COPY request close", self.request.close, primary_error)

    def __enter__(self) -> _CopyBase:
        self._require_open()
        return self

    def __exit__(self, exc_type: Any, exc: Any, traceback: Any) -> None:
        del traceback
        if exc_type is None and isinstance(self, CopyIn) and not self._io_ended:
            self.finish()
        else:
            self.close()

    def __del__(self) -> None:
        if getattr(self, "_closed", True):
            return
        _warn_resource_leak(
            f"unclosed PostGamma {type(self).__name__}", self
        )
        try:
            self.close()
        except BaseException as cleanup_error:
            _warn_preserved_cleanup_error(
                "abandoned COPY close", cleanup_error, None
            )


class CopyIn(_CopyBase):
    """Partial, backpressure-aware COPY FROM STDIN byte writer."""

    def write(self, data: Any) -> int:
        """Write all supplied bytes while honoring native partial progress."""

        self._require_open()
        view = memoryview(data).cast("B")
        offset = 0
        while offset < len(view):
            try:
                consumed, state = _native.copy_write(self._handle, view[offset:])
            except _native.NativeError as error:
                raise_translated(error)
            consumed = int(consumed)
            if consumed < 0 or consumed > len(view) - offset:
                raise InterfaceError("native COPY writer returned an invalid count")
            offset += consumed
            if state == _native.IO_AGAIN:
                if consumed != 0:
                    raise InterfaceError(
                        "native COPY writer reported progress and backpressure"
                    )
                self._progress_wait()
            elif state == _native.IO_PROGRESS:
                if consumed == 0 and offset < len(view):
                    raise InterfaceError("native COPY writer made no progress")
            else:
                raise InterfaceError("native COPY writer returned an invalid state")
        return offset

    def finish(self, failure_message: str | None = None) -> Result:
        """Finish COPY IN and return PostgreSQL's terminal command result."""

        self._require_open()
        while True:
            try:
                state = int(
                    _native.copy_finish(
                        self._handle, failure_message=failure_message
                    )
                )
            except _native.NativeError as error:
                raise_translated(error)
            if state == _native.IO_END:
                break
            if state != _native.IO_AGAIN:
                raise InterfaceError("native COPY finish returned an invalid state")
            self._progress_wait()
        self._io_ended = True
        self._release_copy()
        return self._finish_request()

    def copy_from(self, source: Any, *, chunk_size: int = 64 * 1024) -> Result:
        """Copy from a readable binary object until it returns end of input."""

        chunk_size = _positive_int(chunk_size, "chunk_size")
        while True:
            chunk = source.read(chunk_size)
            if not chunk:
                break
            self.write(chunk)
        return self.finish()


class CopyOut(_CopyBase):
    """Partial, backpressure-aware COPY TO STDOUT byte reader."""

    def read(self, size: int = 64 * 1024) -> bytes:
        """Read up to ``size`` COPY OUT bytes, returning empty bytes at end."""

        self._require_open()
        size = _positive_int(size, "size")
        if self._io_ended:
            self._release_copy()
            self._finish_request()
            return b""
        while True:
            try:
                data, state = _native.copy_read(self._handle, capacity=size)
            except _native.NativeError as error:
                raise_translated(error)
            if data:
                if state != _native.IO_PROGRESS:
                    raise InterfaceError(
                        "native COPY reader returned data with an invalid state"
                    )
                return bytes(data)
            if state == _native.IO_END:
                self._io_ended = True
                self._release_copy()
                self._finish_request()
                return b""
            if state != _native.IO_AGAIN:
                raise InterfaceError("native COPY reader returned an invalid state")
            self._progress_wait()

    def copy_to(self, target: Any, *, chunk_size: int = 64 * 1024) -> Result:
        """Copy all output to a writable binary object and return the result."""

        chunk_size = _positive_int(chunk_size, "chunk_size")
        while True:
            chunk = self.read(chunk_size)
            if not chunk:
                break
            pending = memoryview(chunk)
            while pending:
                consumed = target.write(pending)
                if consumed is None:
                    break
                if not isinstance(consumed, int) or consumed <= 0:
                    raise BlockingIOError("COPY target made no write progress")
                pending = pending[consumed:]
        assert self.command_result is not None
        return self.command_result


def start_copy(
    connection: Connection, sql: str, *, timeout: float | None
) -> CopyIn | CopyOut:
    request = Request.start(connection, sql, timeout=timeout)
    try:
        result = request.next_result()
    except BaseException as primary_error:
        _cleanup_preserving("COPY-start request close", request.close, primary_error)
        raise
    if not isinstance(result, (CopyIn, CopyOut)):
        request.close()
        raise ProgrammingError("SQL did not enter COPY FROM STDIN or COPY TO STDOUT")
    return result


class PreparedStatement:
    """A reusable prepared statement owned by one logical connection."""

    __slots__ = (
        "connection",
        "sql",
        "_handle",
        "_closed",
        "_description",
        "__weakref__",
    )

    def __init__(self, connection: Connection, sql: str, handle: Any) -> None:
        self.connection = connection
        self.sql = sql
        self._handle = handle
        self._closed = False
        self._description: StatementDescription | None = None
        connection._statements.add(self)

    @classmethod
    def prepare(
        cls,
        connection: Connection,
        sql: str,
        parameter_type_oids: Sequence[int] | None,
    ) -> PreparedStatement:
        """Prepare SQL with optional declared parameter type OIDs."""

        try:
            handle = _native.statement_prepare(
                connection._handle,
                sql,
                parameter_type_oids=parameter_type_oids,
            )
        except _native.NativeError as error:
            raise_translated(error)
        return cls(connection, sql, handle)

    @property
    def closed(self) -> bool:
        """Return whether this prepared statement is closed."""

        return self._closed

    @property
    def description(self) -> StatementDescription:
        """Describe parameter types and result columns, caching the result."""

        if self._closed:
            raise InterfaceError("PostGamma prepared statement is closed")
        if self._description is None:
            try:
                native = _native.statement_describe(self._handle)
            except _native.NativeError as error:
                raise_translated(error)
            self._description = StatementDescription(
                tuple(int(oid) for oid in native["parameter_type_oids"]),
                tuple(Column(*column) for column in native["columns"]),
            )
        return self._description

    def execute(
        self,
        parameters: Sequence[Any] | None = None,
        *,
        timeout: float | None = None,
        on_notice: Any | None = None,
    ) -> Result:
        """Execute this statement and return one materialized result."""

        with self.connection._operation_lock:
            self.connection._require_open()
            if self._closed:
                raise InterfaceError("PostGamma prepared statement is closed")
            effective = (
                self.connection.timeout
                if timeout is None
                else _normalize_timeout(timeout)
            )
            self.connection._begin_transaction_if_needed(self.sql, effective)
            request = Request.from_statement(
                self,
                parameters,
                chunk_rows=0,
                timeout=effective,
                result_buffer_limit=0,
                on_notice=on_notice,
            )
        primary_error: BaseException | None = None
        try:
            results = list(request.results())
        except BaseException as error:
            primary_error = error
            raise
        finally:
            _cleanup_preserving(
                "prepared request close", request.close, primary_error
            )
        if len(results) != 1:
            raise InterfaceError(
                "prepared statement returned an invalid result sequence"
            )
        return results[0]

    def stream(
        self,
        parameters: Sequence[Any] | None = None,
        *,
        chunk_rows: int = 1024,
        timeout: float | None = None,
        result_buffer_limit: int = 0,
        on_notice: Any | None = None,
    ) -> RowStream:
        """Execute this statement as a bounded row stream."""

        chunk_rows = _positive_int(chunk_rows, "chunk_rows")
        with self.connection._operation_lock:
            self.connection._require_open()
            if self._closed:
                raise InterfaceError("PostGamma prepared statement is closed")
            effective = (
                self.connection.timeout
                if timeout is None
                else _normalize_timeout(timeout)
            )
            self.connection._begin_transaction_if_needed(self.sql, effective)
            request = Request.from_statement(
                self,
                parameters,
                chunk_rows=chunk_rows,
                timeout=effective,
                result_buffer_limit=_nonnegative_int(
                    result_buffer_limit, "result_buffer_limit"
                ),
                on_notice=on_notice,
            )
        return RowStream.from_request(request)

    def close(self) -> None:
        """Release this statement from its owning logical connection."""

        with self.connection._operation_lock:
            if self._closed:
                return
            with self.connection._request_gate:
                try:
                    _native.statement_close(self._handle)
                except _native.NativeError as error:
                    raise_translated(error)
            self._handle = None
            self._closed = True
            self.connection._statements.discard(self)

    def __enter__(self) -> PreparedStatement:
        if self._closed:
            raise InterfaceError("PostGamma prepared statement is closed")
        return self

    def __exit__(self, exc_type: Any, exc: Any, traceback: Any) -> None:
        del exc_type, exc, traceback
        self.close()

    def __del__(self) -> None:
        if getattr(self, "_handle", None) is not None and not getattr(
            self, "_closed", True
        ):
            _warn_resource_leak("unclosed PostGamma PreparedStatement", self)
