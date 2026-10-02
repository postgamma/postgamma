"""Asyncio adapters driven by PostGamma's stable level-triggered waitables."""

from __future__ import annotations

import asyncio
import os
import time
import weakref
from collections.abc import AsyncIterator, Iterable, Mapping, Sequence
from typing import Any

from . import _native
from ._api import (
    Connection,
    Database,
    OpenMode,
    Result,
    _DEFAULT_TIMEOUT_SECONDS,
    _cleanup_preserving,
    _nonnegative_int,
    _normalize_timeout,
    _positive_int,
    _warn_preserved_cleanup_error,
    _warn_resource_leak,
    connect as sync_connect,
)
from ._errors import (
    InterfaceError,
    ProgrammingError,
    QueryCanceledError,
    QueryTimeoutError,
    raise_translated,
)
from ._management import Operation, OperationProgress, OperationState
from ._sql import is_transaction_control
from ._stream import CopyIn, CopyOut, Request


class _FDHub:
    """Multiplex every waiter for one descriptor through one loop reader."""

    def __init__(self, descriptor: int) -> None:
        self.descriptor = descriptor
        self.waiters: set[asyncio.Future[None]] = set()
        self.registered = False

    def _wake(self, loop: asyncio.AbstractEventLoop) -> None:
        waiters = tuple(self.waiters)
        self.waiters.clear()
        if self.registered:
            loop.remove_reader(self.descriptor)
            self.registered = False
        for waiter in waiters:
            if not waiter.done():
                waiter.set_result(None)

    def _ready(self) -> None:
        self._wake(asyncio.get_running_loop())

    def interrupt(self, loop: asyncio.AbstractEventLoop) -> None:
        """Wake every waiter after removing the live descriptor reader."""

        self._wake(loop)

    async def wait(self, timeout: float | None) -> None:
        loop = asyncio.get_running_loop()
        future: asyncio.Future[None] = loop.create_future()
        self.waiters.add(future)
        if not self.registered:
            try:
                loop.add_reader(self.descriptor, self._ready)
            except (AttributeError, NotImplementedError) as error:
                self.waiters.discard(future)
                _discard_hub(loop, self.descriptor, self)
                raise InterfaceError(
                    "the active asyncio loop does not support POSIX fd readers"
                ) from error
            self.registered = True
        try:
            if timeout is None:
                await future
            else:
                await asyncio.wait_for(future, timeout)
        finally:
            self.waiters.discard(future)
            if not self.waiters and self.registered:
                loop.remove_reader(self.descriptor)
                self.registered = False
            _discard_hub(loop, self.descriptor, self)


_LOOP_HUBS: weakref.WeakKeyDictionary[
    asyncio.AbstractEventLoop, dict[int, _FDHub]
] = weakref.WeakKeyDictionary()


def _hub(descriptor: int) -> _FDHub:
    loop = asyncio.get_running_loop()
    hubs = _LOOP_HUBS.setdefault(loop, {})
    return hubs.setdefault(descriptor, _FDHub(descriptor))


def _discard_hub(
    loop: asyncio.AbstractEventLoop, descriptor: int, hub: _FDHub
) -> None:
    hubs = _LOOP_HUBS.get(loop)
    if (
        hubs is None
        or hubs.get(descriptor) is not hub
        or hub.waiters
        or hub.registered
    ):
        return
    del hubs[descriptor]
    if not hubs:
        del _LOOP_HUBS[loop]


def _interrupt_hub(descriptor: int) -> None:
    loop = asyncio.get_running_loop()
    hubs = _LOOP_HUBS.get(loop)
    if hubs is None:
        return
    hub = hubs.get(descriptor)
    if hub is None:
        return
    hub.interrupt(loop)
    _discard_hub(loop, descriptor, hub)


def _request_remaining(request: Request) -> float | None:
    remaining = request._remaining()
    if remaining is not None and remaining <= 0:
        _request_timeout(request)
    return remaining


def _request_timeout(request: Request) -> None:
    timeout_error = QueryTimeoutError(
        "PostGamma request exceeded its host-side deadline",
        status=_native.PGM_STATUS_TIMEOUT,
        status_name="timeout",
    )
    try:
        request.cancel()
    except BaseException as cleanup_error:
        _warn_preserved_cleanup_error(
            "timed-out request cancellation", cleanup_error, timeout_error
        )
    _cleanup_preserving("timed-out request close", request.close, timeout_error)
    raise timeout_error


async def _await_request_result(
    request: Request,
) -> Result | CopyIn | CopyOut | None:
    try:
        while True:
            availability, result = request.try_next()
            if availability == _native.AVAILABILITY_READY:
                return result
            if availability == _native.AVAILABILITY_END:
                return None
            request.progress()
            try:
                await _hub(request.waitable).wait(_request_remaining(request))
            except asyncio.TimeoutError:
                _request_timeout(request)
    except asyncio.CancelledError as primary_error:
        try:
            request.cancel()
        except BaseException as cleanup_error:
            _warn_preserved_cleanup_error(
                "canceled request cancellation", cleanup_error, primary_error
            )
        _cleanup_preserving(
            "canceled request close", request.close, primary_error
        )
        raise


async def _collect_request(request: Request) -> list[Result]:
    results: list[Result] = []
    primary_error: BaseException | None = None
    try:
        while True:
            result = await _await_request_result(request)
            if result is None:
                return results
            if not isinstance(result, Result):
                result.close()
                raise ProgrammingError("COPY requires the async COPY API")
            results.append(result)
    except BaseException as error:
        primary_error = error
        raise
    finally:
        _cleanup_preserving("async request close", request.close, primary_error)


class AsyncDatabase:
    """Asyncio facade over a lazy :class:`postgamma.Database`."""

    def __init__(
        self,
        database: Database | str | os.PathLike[str],
        **options: Any,
    ) -> None:
        if isinstance(database, Database):
            if options:
                raise TypeError("options cannot accompany an existing Database")
            self._database = database
            self._owns_database = False
        else:
            self._database = Database(database, **options)
            self._owns_database = True
        self._closing = False

    @property
    def sync_database(self) -> Database:
        """Return the synchronous database object wrapped by this facade."""

        return self._database

    async def open(self) -> AsyncDatabase:
        """Open the database without blocking the asyncio event loop."""

        await asyncio.to_thread(self._database.open)
        return self

    async def connect(self, **options: Any) -> AsyncConnection:
        """Open one logical connection and return its asyncio facade."""

        connection = await asyncio.to_thread(self._database.connect, **options)
        return AsyncConnection(connection)

    async def execute(
        self,
        sql: str,
        parameters: Sequence[Any] | None = None,
        *,
        timeout: float | None = None,
    ) -> Result:
        """Execute one autocommitted statement on a temporary connection."""

        connection = await self.connect(autocommit=True, timeout=timeout)
        primary_error: BaseException | None = None
        try:
            return await connection.execute(sql, parameters, timeout=timeout)
        except BaseException as error:
            primary_error = error
            raise
        finally:
            try:
                await connection.close()
            except BaseException as cleanup_error:
                if primary_error is None:
                    raise
                _warn_preserved_cleanup_error(
                    "async connection close", cleanup_error, primary_error
                )

    async def events(self) -> AsyncIterator[Any]:
        """Yield instance events without occupying a worker thread."""

        await asyncio.to_thread(self._database.open)
        while not self._closing and not self._database.closed:
            try:
                event = self._database.next_event(timeout=0)
            except InterfaceError:
                if self._closing or self._database.closed:
                    return
                raise
            if event is not None:
                yield event
                continue
            try:
                descriptor = self._database.waitable
            except InterfaceError:
                if self._closing or self._database.closed:
                    return
                raise
            try:
                await _hub(descriptor).wait(0.25)
            except asyncio.TimeoutError:
                pass

    async def checkpoint(self, *, timeout: float | None = None) -> OperationProgress:
        """Run an in-process checkpoint and return its final progress snapshot."""

        operation = self._database.checkpoint(wait=False)
        return await wait_operation(operation, timeout=timeout)

    async def maintenance(
        self,
        kind: Any = "vacuum_analyze",
        *,
        timeout: float | None = None,
        **options: Any,
    ) -> OperationProgress:
        """Run one supported maintenance command to completion."""

        operation = self._database.maintenance(kind, wait=False, **options)
        return await wait_operation(operation, timeout=timeout)

    async def logical_dump(
        self, stream: Any, *, timeout: float | None = None, **options: Any
    ) -> OperationProgress:
        """Write a logical archive through an asynchronous operation driver."""

        operation = self._database.logical_dump(stream, wait=False, **options)
        return await wait_operation(operation, timeout=timeout)

    async def logical_restore(
        self, stream: Any, *, timeout: float | None = None, **options: Any
    ) -> OperationProgress:
        """Restore a logical archive through an asynchronous operation driver."""

        operation = self._database.logical_restore(stream, wait=False, **options)
        return await wait_operation(operation, timeout=timeout)

    async def close(self, **options: Any) -> None:
        """Close the wrapped database when this facade owns it."""

        if self._database.closed:
            return
        self._closing = True
        try:
            if self._database.opened:
                try:
                    _interrupt_hub(self._database.waitable)
                except InterfaceError:
                    if not self._database.closed:
                        raise
            await asyncio.to_thread(self._database.close, **options)
        except BaseException:
            self._closing = False
            raise

    async def __aenter__(self) -> AsyncDatabase:
        return await self.open()

    async def __aexit__(self, exc_type: Any, exc: Any, traceback: Any) -> None:
        del exc_type, exc, traceback
        if self._owns_database or not self._database.closed:
            await self.close()


class AsyncConnection:
    """One serialized asyncio session; other sessions still run concurrently."""

    def __init__(self, connection: Connection) -> None:
        self._connection = connection
        self._lock = asyncio.Lock()

    @property
    def sync_connection(self) -> Connection:
        """Return the synchronous logical connection wrapped by this facade."""

        return self._connection

    @property
    def closed(self) -> bool:
        """Return whether the wrapped connection is closed."""

        return self._connection.closed

    async def _execute_locked(
        self,
        sql: str,
        parameters: Sequence[Any] | None,
        timeout: float | None,
    ) -> Result:
        request = Request.start(
            self._connection,
            sql,
            parameters,
            timeout=timeout,
        )
        results = await _collect_request(request)
        if len(results) != 1:
            raise InterfaceError("query returned an invalid result sequence")
        return results[0]

    async def _begin_if_needed(self, sql: str, timeout: float | None) -> None:
        if (
            not self._connection.autocommit
            and not is_transaction_control(sql)
            and self._connection.transaction_status == _native.TRANSACTION_IDLE
        ):
            await self._execute_locked("BEGIN", None, timeout)

    async def execute(
        self,
        sql: str,
        parameters: Sequence[Any] | None = None,
        *,
        timeout: float | None = None,
    ) -> Result:
        """Execute one statement through a caller-driven native request."""

        effective = (
            self._connection.timeout
            if timeout is None
            else _normalize_timeout(timeout)
        )
        async with self._lock:
            self._connection._require_open()
            await self._begin_if_needed(sql, effective)
            return await self._execute_locked(sql, parameters, effective)

    async def stream(
        self,
        sql: str,
        parameters: Sequence[Any] | None = None,
        *,
        chunk_rows: int = 1024,
        timeout: float | None = None,
        result_buffer_limit: int = 0,
        on_notice: Any | None = None,
    ) -> AsyncRowStream:
        """Execute SQL and return an asynchronous bounded row stream."""

        chunk_rows = _positive_int(chunk_rows, "chunk_rows")
        effective = (
            self._connection.timeout
            if timeout is None
            else _normalize_timeout(timeout)
        )
        await self._lock.acquire()
        try:
            self._connection._require_open()
            await self._begin_if_needed(sql, effective)
            request = Request.start(
                self._connection,
                sql,
                parameters,
                chunk_rows=chunk_rows,
                timeout=effective,
                result_buffer_limit=result_buffer_limit,
                on_notice=on_notice,
            )
            return AsyncRowStream(request, self._lock.release)
        except BaseException:
            self._lock.release()
            raise

    async def execute_script(
        self, sql: str, *, timeout: float | None = None
    ) -> tuple[Result, ...]:
        """Execute simple-protocol SQL and return every ordered result."""

        effective = (
            self._connection.timeout
            if timeout is None
            else _normalize_timeout(timeout)
        )
        async with self._lock:
            request = Request.start(
                self._connection, sql, timeout=effective, script=True
            )
            return tuple(await _collect_request(request))

    async def prepare(
        self,
        sql: str,
        *,
        parameter_type_oids: Sequence[int] | None = None,
    ) -> AsyncPreparedStatement:
        """Prepare SQL and return an asynchronous statement facade."""

        async with self._lock:
            statement = await asyncio.to_thread(
                self._connection.prepare,
                sql,
                parameter_type_oids=parameter_type_oids,
            )
            return AsyncPreparedStatement(self, statement)

    async def copy(self, sql: str, *, timeout: float | None = None) -> AsyncCopy:
        """Start COPY IN or COPY OUT and return an asynchronous byte stream."""

        effective = (
            self._connection.timeout
            if timeout is None
            else _normalize_timeout(timeout)
        )
        await self._lock.acquire()
        try:
            self._connection._require_open()
            await self._begin_if_needed(sql, effective)
            request = Request.start(self._connection, sql, timeout=effective)
            result = await _await_request_result(request)
            if not isinstance(result, (CopyIn, CopyOut)):
                request.close()
                raise ProgrammingError("SQL did not enter COPY mode")
            return AsyncCopy(result, self._lock.release)
        except BaseException:
            self._lock.release()
            raise

    async def commit(self) -> None:
        """Commit the current transaction."""

        async with self._lock:
            self._connection._require_open()
            if self._connection._transaction_scope_active:
                raise ProgrammingError(
                    "transaction() owns commit; leave its context to commit"
                )
            if self._connection.transaction_status != _native.TRANSACTION_IDLE:
                await self._execute_locked("COMMIT", None, self._connection.timeout)

    async def rollback(self) -> None:
        """Roll back the current transaction."""

        async with self._lock:
            self._connection._require_open()
            if self._connection._transaction_scope_active:
                raise ProgrammingError(
                    "transaction() owns rollback; raise from its context to roll back"
                )
            if self._connection.transaction_status != _native.TRANSACTION_IDLE:
                await self._execute_locked("ROLLBACK", None, self._connection.timeout)

    def transaction(self) -> _AsyncTransactionContext:
        """Return a single-use asynchronous transaction scope."""

        return _AsyncTransactionContext(self)

    async def _enter_transaction_scope(self) -> None:
        async with self._lock:
            self._connection._claim_transaction_scope()
            try:
                await self._execute_locked(
                    "BEGIN", None, self._connection.timeout
                )
            except BaseException:
                self._connection._release_transaction_scope()
                raise

    async def _exit_transaction_scope(
        self, *, commit: bool, primary_error: BaseException | None
    ) -> None:
        try:
            async with self._lock:
                self._connection._require_open()
                if self._connection.transaction_status != _native.TRANSACTION_IDLE:
                    try:
                        await self._execute_locked(
                            "COMMIT" if commit else "ROLLBACK",
                            None,
                            self._connection.timeout,
                        )
                    except BaseException as cleanup_error:
                        if primary_error is None:
                            raise
                        _warn_preserved_cleanup_error(
                            "async transaction-scope rollback",
                            cleanup_error,
                            primary_error,
                        )
        finally:
            self._connection._release_transaction_scope()

    def cancel(self) -> bool:
        """Request cancellation of this connection's active request."""

        return self._connection.cancel()

    async def close(self, **options: Any) -> None:
        """Close the logical connection without blocking the event loop."""

        async with self._lock:
            await asyncio.to_thread(self._connection.close, **options)

    async def __aenter__(self) -> AsyncConnection:
        self._connection._require_open()
        return self

    async def __aexit__(self, exc_type: Any, exc: Any, traceback: Any) -> None:
        del traceback
        if exc_type is None:
            try:
                await self.commit()
            except BaseException as primary_error:
                try:
                    await self.close()
                except BaseException as cleanup_error:
                    _warn_preserved_cleanup_error(
                        "async connection close", cleanup_error, primary_error
                    )
                raise
            await self.close()
            return
        try:
            await self.rollback()
        except BaseException as cleanup_error:
            _warn_preserved_cleanup_error(
                "async transaction rollback", cleanup_error, exc
            )
        try:
            await self.close()
        except BaseException as cleanup_error:
            _warn_preserved_cleanup_error(
                "async connection close", cleanup_error, exc
            )


class _AsyncTransactionContext:
    """Single-use transaction scope for one asynchronous connection."""

    def __init__(self, connection: AsyncConnection) -> None:
        self._connection = connection
        self._entered = False
        self._used = False

    async def __aenter__(self) -> AsyncConnection:
        if self._used:
            raise ProgrammingError("a transaction scope is single-use")
        self._used = True
        await self._connection._enter_transaction_scope()
        self._entered = True
        return self._connection

    async def __aexit__(
        self, exc_type: Any, exc: BaseException | None, traceback: Any
    ) -> None:
        del traceback
        if not self._entered:
            return
        try:
            await self._connection._exit_transaction_scope(
                commit=exc_type is None,
                primary_error=exc,
            )
        finally:
            self._entered = False


class AsyncRowStream:
    """Async iterator over bounded result chunks without helper threads."""

    def __init__(self, request: Request, release: Any) -> None:
        self.request = request
        self.columns: tuple[Any, ...] = ()
        self.command_status: str | None = None
        self.rowcount = -1
        self._rows: list[tuple[Any, ...]] = []
        self._rows_seen = 0
        self._release = release
        self._closed = False

    def _finish_close(self) -> None:
        if self._closed:
            return
        self._closed = True
        release = self._release
        self._release = None
        if release is not None:
            release()

    async def fetchone(self) -> tuple[Any, ...] | None:
        """Return the next streamed row without blocking the event loop."""

        try:
            while not self._rows:
                if self._closed:
                    return None
                result = await _await_request_result(self.request)
                if result is None:
                    await self.aclose()
                    return None
                if not isinstance(result, Result):
                    result.close()
                    raise ProgrammingError("row stream received a COPY result")
                if result.columns and not self.columns:
                    self.columns = result.columns
                if result.kind == _native.RESULT_TUPLES_CHUNK:
                    self._rows_seen += len(result.rows)
                    self._rows.extend(result.rows)
                    continue
                self.command_status = result.command_status
                self._rows_seen += len(result.rows)
                self.rowcount = self._rows_seen
                self._rows.extend(result.rows)
            return self._rows.pop(0)
        except BaseException as primary_error:
            try:
                await self.aclose()
            except BaseException as cleanup_error:
                _warn_preserved_cleanup_error(
                    "async row-stream close", cleanup_error, primary_error
                )
            raise

    async def fetchmany(self, size: int = 1) -> list[tuple[Any, ...]]:
        """Return up to ``size`` rows from the bounded stream."""

        size = _nonnegative_int(size, "size")
        rows: list[tuple[Any, ...]] = []
        while len(rows) < size:
            row = await self.fetchone()
            if row is None:
                break
            rows.append(row)
        return rows

    async def aclose(self) -> None:
        """Cancel or retire the request and close this stream."""

        if self._closed:
            return
        self.request.close()
        self._finish_close()

    def __aiter__(self) -> AsyncRowStream:
        return self

    async def __anext__(self) -> tuple[Any, ...]:
        row = await self.fetchone()
        if row is None:
            raise StopAsyncIteration
        return row

    async def __aenter__(self) -> AsyncRowStream:
        return self

    async def __aexit__(self, exc_type: Any, exc: Any, traceback: Any) -> None:
        del exc_type, exc, traceback
        await self.aclose()

    def __del__(self) -> None:
        if getattr(self, "_closed", True):
            return
        _warn_resource_leak("unclosed PostGamma AsyncRowStream", self)
        try:
            self.request.close()
        except BaseException as cleanup_error:
            _warn_preserved_cleanup_error(
                "abandoned async row-stream close", cleanup_error, None
            )
            return
        try:
            self._finish_close()
        except BaseException as cleanup_error:
            _warn_preserved_cleanup_error(
                "abandoned async row-stream lease release", cleanup_error, None
            )


class AsyncCopy:
    """Async facade over COPY IN or COPY OUT partial I/O."""

    def __init__(self, copy: CopyIn | CopyOut, release: Any) -> None:
        self._copy = copy
        self._release = release
        self._closed = False

    @property
    def direction(self) -> str:
        """Return ``in`` for COPY IN or ``out`` for COPY OUT."""

        return "in" if isinstance(self._copy, CopyIn) else "out"

    async def _abort_preserving(self, primary_error: BaseException) -> None:
        if self._closed:
            return
        try:
            await asyncio.to_thread(self._copy.close)
        except BaseException as cleanup_error:
            _warn_preserved_cleanup_error(
                "async COPY close", cleanup_error, primary_error
            )
        finally:
            self._finish_close()

    async def write(self, data: Any) -> int:
        """Write all supplied bytes to COPY IN with asynchronous progress."""

        if not isinstance(self._copy, CopyIn):
            raise ProgrammingError("COPY TO STDOUT is not writable")
        view = memoryview(data).cast("B")
        offset = 0
        try:
            while offset < len(view):
                consumed, state = _native.copy_write(
                    self._copy._handle, view[offset:]
                )
                consumed = int(consumed)
                if consumed < 0 or consumed > len(view) - offset:
                    raise InterfaceError(
                        "native COPY writer returned an invalid count"
                    )
                offset += consumed
                if state == _native.IO_AGAIN:
                    if consumed != 0:
                        raise InterfaceError(
                            "native COPY writer reported progress and backpressure"
                        )
                    self._copy.request.progress()
                    await _hub(self._copy.request.waitable).wait(
                        _request_remaining(self._copy.request)
                    )
                elif state != _native.IO_PROGRESS:
                    raise InterfaceError(
                        "native COPY writer returned an invalid state"
                    )
                elif consumed == 0 and offset < len(view):
                    raise InterfaceError("native COPY writer made no progress")
            return offset
        except _native.NativeError as error:
            try:
                raise_translated(error)
            except BaseException as primary_error:
                await self._abort_preserving(primary_error)
                raise
        except BaseException as primary_error:
            await self._abort_preserving(primary_error)
            raise

    async def finish(self) -> Result:
        """Finish COPY IN and return PostgreSQL's command result."""

        if not isinstance(self._copy, CopyIn):
            raise ProgrammingError("COPY TO STDOUT cannot be finished as input")
        try:
            while True:
                try:
                    state = int(_native.copy_finish(self._copy._handle))
                except _native.NativeError as error:
                    raise_translated(error)
                if state == _native.IO_END:
                    break
                if state != _native.IO_AGAIN:
                    raise InterfaceError("native COPY finish returned an invalid state")
                self._copy.request.progress()
                await _hub(self._copy.request.waitable).wait(
                    _request_remaining(self._copy.request)
                )
            self._copy._io_ended = True
            self._copy._release_copy()
            results = await _collect_request(self._copy.request)
            if len(results) != 1:
                raise InterfaceError("COPY returned an invalid result sequence")
            self._copy.command_result = results[0]
            result = results[0]
            self._finish_close()
            return result
        except _native.NativeError as error:
            try:
                raise_translated(error)
            except BaseException as primary_error:
                await self._abort_preserving(primary_error)
                raise
        except BaseException as primary_error:
            await self._abort_preserving(primary_error)
            raise

    async def copy_from(
        self, source: Any, *, chunk_size: int = 64 * 1024
    ) -> Result:
        """Copy from a readable binary object until end of input."""

        if not isinstance(self._copy, CopyIn):
            raise ProgrammingError("COPY TO STDOUT has no input source")
        chunk_size = _positive_int(chunk_size, "chunk_size")
        try:
            while True:
                chunk = source.read(chunk_size)
                if not chunk:
                    break
                await self.write(chunk)
            return await self.finish()
        except BaseException as primary_error:
            await self._abort_preserving(primary_error)
            raise

    async def read(self, size: int = 64 * 1024) -> bytes:
        """Read up to ``size`` COPY OUT bytes, or empty bytes at end."""

        if not isinstance(self._copy, CopyOut):
            raise ProgrammingError("COPY FROM STDIN is not readable")
        size = _positive_int(size, "size")
        try:
            while True:
                if self._copy._io_ended:
                    self._copy._release_copy()
                    results = await _collect_request(self._copy.request)
                    if len(results) != 1:
                        raise InterfaceError(
                            "COPY returned an invalid result sequence"
                        )
                    self._copy.command_result = results[0]
                    self._finish_close()
                    return b""
                data, state = _native.copy_read(
                    self._copy._handle, capacity=size
                )
                if state == _native.IO_END:
                    self._copy._io_ended = True
                if data:
                    if state != _native.IO_PROGRESS:
                        raise InterfaceError(
                            "native COPY reader returned data with an invalid state"
                        )
                    return bytes(data)
                if self._copy._io_ended:
                    continue
                if state != _native.IO_AGAIN:
                    raise InterfaceError(
                        "native COPY reader returned an invalid state"
                    )
                self._copy.request.progress()
                await _hub(self._copy.request.waitable).wait(
                    _request_remaining(self._copy.request)
                )
        except _native.NativeError as error:
            try:
                raise_translated(error)
            except BaseException as primary_error:
                await self._abort_preserving(primary_error)
                raise
        except BaseException as primary_error:
            await self._abort_preserving(primary_error)
            raise

    async def copy_to(
        self, target: Any, *, chunk_size: int = 64 * 1024
    ) -> Result:
        """Copy all output to a writable binary object."""

        if not isinstance(self._copy, CopyOut):
            raise ProgrammingError("COPY FROM STDIN has no output target")
        chunk_size = _positive_int(chunk_size, "chunk_size")
        try:
            while True:
                chunk = await self.read(chunk_size)
                if not chunk:
                    break
                pending = memoryview(chunk)
                while pending:
                    consumed = target.write(pending)
                    if consumed is None:
                        break
                    if not isinstance(consumed, int) or consumed <= 0:
                        raise BlockingIOError(
                            "COPY target made no write progress"
                        )
                    pending = pending[consumed:]
            if self._copy.command_result is None:
                raise InterfaceError(
                    "COPY output completed without a command result"
                )
            return self._copy.command_result
        except BaseException as primary_error:
            await self._abort_preserving(primary_error)
            raise

    def _finish_close(self) -> None:
        if not self._closed:
            self._closed = True
            release = self._release
            self._release = None
            if release is not None:
                release()

    async def aclose(self) -> None:
        """Abort or retire the active COPY operation and release its request."""

        if self._closed:
            return
        try:
            await asyncio.to_thread(self._copy.close)
        finally:
            self._finish_close()

    async def __aenter__(self) -> AsyncCopy:
        return self

    async def __aexit__(self, exc_type: Any, exc: Any, traceback: Any) -> None:
        del exc_type, exc, traceback
        await self.aclose()

    def __del__(self) -> None:
        if getattr(self, "_closed", True):
            return
        _warn_resource_leak("unclosed PostGamma AsyncCopy", self)
        try:
            self._copy.close()
        except BaseException as cleanup_error:
            _warn_preserved_cleanup_error(
                "abandoned async COPY close", cleanup_error, None
            )
            return
        try:
            self._finish_close()
        except BaseException as cleanup_error:
            _warn_preserved_cleanup_error(
                "abandoned async COPY lease release", cleanup_error, None
            )


class AsyncPreparedStatement:
    """Asyncio execution facade for one reusable prepared statement."""

    def __init__(self, connection: AsyncConnection, statement: Any) -> None:
        self.connection = connection
        self.statement = statement

    @property
    def description(self) -> Any:
        """Return the cached or lazily loaded statement description."""

        return self.statement.description

    async def execute(
        self,
        parameters: Sequence[Any] | None = None,
        *,
        timeout: float | None = None,
        on_notice: Any | None = None,
    ) -> Result:
        """Execute the prepared statement and materialize one result."""

        effective = (
            self.connection._connection.timeout
            if timeout is None
            else _normalize_timeout(timeout)
        )
        async with self.connection._lock:
            await self.connection._begin_if_needed(self.statement.sql, effective)
            request = Request.from_statement(
                self.statement,
                parameters,
                chunk_rows=0,
                timeout=effective,
                result_buffer_limit=0,
                on_notice=on_notice,
            )
            results = await _collect_request(request)
        if len(results) != 1:
            raise InterfaceError(
                "prepared statement returned an invalid result sequence"
            )
        return results[0]

    async def stream(
        self,
        parameters: Sequence[Any] | None = None,
        *,
        chunk_rows: int = 1024,
        timeout: float | None = None,
        result_buffer_limit: int = 0,
        on_notice: Any | None = None,
    ) -> AsyncRowStream:
        """Execute the prepared statement as a bounded asynchronous stream."""

        chunk_rows = _positive_int(chunk_rows, "chunk_rows")
        effective = (
            self.connection._connection.timeout
            if timeout is None
            else _normalize_timeout(timeout)
        )
        await self.connection._lock.acquire()
        try:
            await self.connection._begin_if_needed(self.statement.sql, effective)
            request = Request.from_statement(
                self.statement,
                parameters,
                chunk_rows=chunk_rows,
                timeout=effective,
                result_buffer_limit=result_buffer_limit,
                on_notice=on_notice,
            )
            return AsyncRowStream(request, self.connection._lock.release)
        except BaseException:
            self.connection._lock.release()
            raise

    async def close(self) -> None:
        """Close the underlying prepared statement."""

        async with self.connection._lock:
            await asyncio.to_thread(self.statement.close)

    async def __aenter__(self) -> AsyncPreparedStatement:
        return self

    async def __aexit__(self, exc_type: Any, exc: Any, traceback: Any) -> None:
        del exc_type, exc, traceback
        await self.close()


async def wait_operation(
    operation: Operation, *, timeout: float | None = None
) -> OperationProgress:
    normalized = (
        operation.database.timeout if timeout is None else _normalize_timeout(timeout)
    )
    deadline = None if normalized is None else time.monotonic() + normalized
    primary_error: BaseException | None = None
    try:
        while True:
            state = operation.progress()
            snapshot = operation.snapshot
            if state is OperationState.COMPLETED:
                return snapshot
            if state is OperationState.CANCELED:
                raise QueryCanceledError(
                    "PostGamma management operation was canceled",
                    status=_native.PGM_STATUS_CANCELED,
                    status_name="canceled",
                )
            if state is OperationState.FAILED:
                raise InterfaceError("PostGamma management operation failed")
            remaining = None if deadline is None else deadline - time.monotonic()
            if remaining is not None and remaining <= 0:
                operation.cancel()
                raise QueryTimeoutError(
                    "PostGamma management operation exceeded its deadline",
                    status=_native.PGM_STATUS_TIMEOUT,
                    status_name="timeout",
                )
            try:
                await _hub(operation.waitable).wait(remaining)
            except asyncio.TimeoutError:
                operation.cancel()
                raise QueryTimeoutError(
                    "PostGamma management operation exceeded its deadline",
                    status=_native.PGM_STATUS_TIMEOUT,
                    status_name="timeout",
                ) from None
    except asyncio.CancelledError as error:
        primary_error = error
        try:
            operation.cancel()
        except BaseException as cleanup_error:
            _warn_preserved_cleanup_error(
                "canceled operation cancellation", cleanup_error, error
            )
        raise
    except BaseException as error:
        primary_error = error
        raise
    finally:
        _cleanup_preserving(
            "async management operation close", operation.close, primary_error
        )


async def connect_async(
    path: str | os.PathLike[str],
    *,
    mode: OpenMode | str = OpenMode.OPEN_OR_CREATE,
    database: str = "postgres",
    user: str = "postgamma",
    application_name: str = "postgamma-python",
    settings: Mapping[str, Any] | Iterable[tuple[str, Any]] | None = None,
    connection_settings: (
        Mapping[str, Any] | Iterable[tuple[str, Any]] | None
    ) = None,
    worker_count: int = 4,
    autocommit: bool = False,
    timeout: float | None = _DEFAULT_TIMEOUT_SECONDS,
    resource_root: str | os.PathLike[str] | None = None,
    executable_path: str | os.PathLike[str] | None = None,
) -> AsyncConnection:
    """Open one logical session without blocking the asyncio event loop.

    This is the asynchronous counterpart of :func:`postgamma.connect`.  It
    accepts the same options, shares the same process-local instance registry,
    and returns an already-open :class:`AsyncConnection`.

    Args:
        path: Persistent PostgreSQL cluster-directory path.
        mode: ``open_or_create``, ``open_existing``, or ``create_new``.
        database: Existing PostgreSQL logical database inside the cluster.
        user: Existing PostgreSQL role for the logical session.
        application_name: PostgreSQL application name for the session.
        settings: Instance/startup PostgreSQL settings. Repeated connections
            must request the same instance settings.
        connection_settings: Session-local PostgreSQL settings.
        worker_count: Carrier workers shared by sessions on this instance.
        autocommit: Commit each statement independently when true.
        timeout: Default operation deadline in seconds, or ``None`` for no
            deadline.
        resource_root: Advanced resource-pack override; wheels resolve it.
        executable_path: Advanced executable-identity override; wheels resolve
            it and do not launch that executable.

    Returns:
        An open asynchronous logical connection whose close releases one
        process-local instance lease.

    Raises:
        OperationalError: The path violates the requested mode or cannot open.
        InterfaceError: Instance options conflict with a live implicit instance.
        DatabaseError: PostgreSQL rejects the requested logical database or role.
    """

    connection = await asyncio.to_thread(
        sync_connect,
        path,
        mode=mode,
        database=database,
        user=user,
        application_name=application_name,
        settings=settings,
        connection_settings=connection_settings,
        worker_count=worker_count,
        autocommit=autocommit,
        timeout=timeout,
        resource_root=resource_root,
        executable_path=executable_path,
    )
    return AsyncConnection(connection)
