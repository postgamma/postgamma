"""High-level and PEP 249 APIs for in-process postgamma."""

from __future__ import annotations

import math
import os
import threading
import warnings
import weakref
from collections import deque
from collections.abc import Iterable, Mapping, Sequence
from dataclasses import dataclass
from enum import Enum
from pathlib import Path
from typing import Any

try:
    from typing import Self
except ImportError:  # pragma: no cover - exercised by the CPython 3.10 wheel job
    from typing import TypeVar

    Self = TypeVar("Self")

from . import _native
from ._codec import RawValue, decode_value, encode_parameters
from ._errors import (
    ForkedProcessError,
    InterfaceError,
    NotSupportedError,
    OperationalError,
    ProgrammingError,
    raise_translated,
)
from ._sql import is_transaction_control, rewrite_numeric_parameters


_DEFAULT_TIMEOUT_SECONDS = 30.0


class OpenMode(str, Enum):
    """Explicit policy for opening a persistent cluster path."""

    OPEN_OR_CREATE = "open_or_create"
    OPEN_EXISTING = "open_existing"
    CREATE_NEW = "create_new"

    @classmethod
    def parse(cls, value: OpenMode | str) -> OpenMode:
        """Normalize an enum member or its public string spelling."""

        if isinstance(value, cls):
            return value
        if not isinstance(value, str):
            raise TypeError("mode must be an OpenMode or string")
        try:
            return cls(value)
        except ValueError as error:
            choices = ", ".join(mode.value for mode in cls)
            raise ValueError(f"mode must be one of: {choices}") from error

    def __str__(self) -> str:
        return self.value


@dataclass(frozen=True, slots=True)
class Column:
    """PostgreSQL result-column metadata independent of PostgreSQL headers."""

    name: str | None
    table_oid: int
    table_column: int
    type_oid: int
    type_size: int
    type_modifier: int
    format: int

    @property
    def dbapi_description(self) -> tuple[Any, ...]:
        """Return this column in the seven-field PEP 249 description form."""

        return (self.name, self.type_oid, None, None, None, None, None)


class Result:
    """A materialized, Python-owned PostgreSQL result."""

    __slots__ = (
        "kind",
        "command_status",
        "columns",
        "_rows",
        "_position",
        "rowcount",
    )

    def __init__(self, native: Mapping[str, Any]) -> None:
        self.kind = int(native["kind"])
        self.command_status = native["command_status"]
        self.columns = tuple(Column(*column) for column in native["columns"])
        self._rows = tuple(
            tuple(
                None if value is None else decode_value(value[0], value[1], value[2])
                for value in row
            )
            for row in native["rows"]
        )
        self._position = 0
        self.rowcount = self._derive_rowcount()

    def _derive_rowcount(self) -> int:
        if self.columns:
            return len(self._rows)
        if not self.command_status:
            return -1
        fields = self.command_status.split()
        if fields and fields[-1].isdigit():
            return int(fields[-1])
        return -1

    @property
    def description(self) -> tuple[tuple[Any, ...], ...] | None:
        """Return PEP 249 column descriptions, or ``None`` without columns."""

        if not self.columns:
            return None
        return tuple(column.dbapi_description for column in self.columns)

    @property
    def rows(self) -> tuple[tuple[Any, ...], ...]:
        """Return all rows owned by this materialized result."""

        return self._rows

    def fetchone(self) -> tuple[Any, ...] | None:
        """Return the next row, or ``None`` after the result is exhausted."""

        if self._position >= len(self._rows):
            return None
        row = self._rows[self._position]
        self._position += 1
        return row

    def fetchmany(self, size: int = 1) -> list[tuple[Any, ...]]:
        """Return up to ``size`` rows from the current result position."""

        if size < 0:
            raise ValueError("fetch size cannot be negative")
        end = min(len(self._rows), self._position + size)
        rows = list(self._rows[self._position : end])
        self._position = end
        return rows

    def fetchall(self) -> list[tuple[Any, ...]]:
        """Return every remaining row and exhaust the result."""

        rows = list(self._rows[self._position :])
        self._position = len(self._rows)
        return rows

    def __iter__(self) -> Self:
        return self

    def __next__(self) -> tuple[Any, ...]:
        row = self.fetchone()
        if row is None:
            raise StopIteration
        return row


class Database:
    """A lazy in-process PostgreSQL instance backed by one cluster directory.

    ``path`` names the persistent PostgreSQL cluster directory, not a DSN,
    server address, SQL database name, or SQLite-style file.  The directory is
    initialized on first open when ``mode`` permits creation and it does not
    already contain a compatible cluster.  Construction alone does not open it.

    Args:
        path: Persistent cluster directory.  A relative path is resolved when
            the instance first opens; its parent directory must already exist.
        mode: Whether to open or create, require an existing cluster, or create
            a new cluster only when the final path does not exist.
        settings: Instance/startup PostgreSQL settings as a mapping or iterable
            of name/value pairs.  Values are converted to strings.
        worker_count: Number of carrier workers available to logical sessions.
            The native PostgreSQL 19 kernel requires at least two.
        timeout: Default operation deadline in seconds, or ``None`` for no
            deadline.
        resource_root: Advanced override for the bundled PostgreSQL resource
            pack.  Installed wheels resolve this automatically.
        executable_path: Advanced PostgreSQL executable-identity override.
            Installed wheels resolve it to ``bin/postgres`` in their private
            resource pack; the embedded lifecycle does not launch it.
        result_buffer_limit: Maximum materialized result bytes, or zero for the
            embedded default.
        maximum_value_size: Maximum individual value bytes, or zero for the
            embedded default.
        event_queue_capacity: Maximum number of routed instance events.

    Note:
        Entering the context, calling :meth:`open`, opening a connection, or
        executing through the database triggers the first open.  A closed
        ``Database`` object cannot be reopened; construct a new object for the
        same persistent path.
    """

    def __init__(
        self,
        path: str | os.PathLike[str],
        *,
        mode: OpenMode | str = OpenMode.OPEN_OR_CREATE,
        settings: Mapping[str, Any] | Iterable[tuple[str, Any]] | None = None,
        worker_count: int = 4,
        timeout: float | None = _DEFAULT_TIMEOUT_SECONDS,
        resource_root: str | os.PathLike[str] | None = None,
        executable_path: str | os.PathLike[str] | None = None,
        result_buffer_limit: int = 0,
        maximum_value_size: int = 0,
        event_queue_capacity: int = 64,
    ) -> None:
        self.path = os.fspath(path)
        self.mode = OpenMode.parse(mode)
        self.settings = _normalize_settings(settings)
        self.worker_count = _positive_int(worker_count, "worker_count")
        self.timeout = _normalize_timeout(timeout)
        self.resource_root = (
            os.fspath(resource_root) if resource_root is not None else None
        )
        self.executable_path = (
            os.fspath(executable_path) if executable_path is not None else None
        )
        self.result_buffer_limit = _nonnegative_int(
            result_buffer_limit, "result_buffer_limit"
        )
        self.maximum_value_size = _nonnegative_int(
            maximum_value_size, "maximum_value_size"
        )
        self.event_queue_capacity = _positive_int(
            event_queue_capacity, "event_queue_capacity"
        )
        self._handle: Any | None = None
        self._owner_pid: int | None = None
        self._closed = False
        self._lock = threading.RLock()
        self._connections: weakref.WeakSet[Connection] = weakref.WeakSet()
        self._operations: weakref.WeakSet[Any] = weakref.WeakSet()
        self._event_handlers: dict[int | None, list[Any]] = {}
        self._request_event_handlers: dict[int, list[Any]] = {}
        self._event_backlog: deque[Any] = deque()

    @property
    def closed(self) -> bool:
        """Return whether this database object is permanently closed."""

        return self._closed

    @property
    def opened(self) -> bool:
        """Return whether the lazy native instance has been opened."""

        return self._handle is not None and not self._closed

    def open(self) -> Self:
        """Create or open the cluster and return this database object.

        With the default ``mode="open_or_create"``, a missing final directory
        is initialized in a private staging directory and published atomically.
        A compatible existing cluster is opened without replacing data.

        Returns:
            This database object after the embedded instance is ready.

        Raises:
            OperationalError: The path is missing when creation is disabled,
                is nonempty but incompatible, is already owned, or cannot be
                initialized or opened.
            ForkedProcessError: The object was inherited across ``fork()``.
        """

        with self._lock:
            self._ensure_open()
        return self

    def _ensure_open(self) -> Any:
        if self._closed:
            raise InterfaceError("PostGamma database is closed")
        if self._owner_pid is not None and self._owner_pid != os.getpid():
            raise ForkedProcessError(
                "PostGamma database was inherited across fork; create a new "
                "Database in the child process"
            )
        if self._handle is not None:
            return self._handle
        resource_root, executable_path = _resolve_runtime_paths(
            self.resource_root, self.executable_path
        )
        try:
            self._handle = _native.instance_open(
                self.path,
                create=self.mode is not OpenMode.OPEN_EXISTING,
                create_new=self.mode is OpenMode.CREATE_NEW,
                executable_path=executable_path,
                resource_root=resource_root,
                settings=self.settings,
                worker_count=self.worker_count,
                result_buffer_limit=self.result_buffer_limit,
                maximum_value_size=self.maximum_value_size,
                event_queue_capacity=self.event_queue_capacity,
            )
        except _native.NativeError as error:
            raise_translated(error)
        self._owner_pid = os.getpid()
        return self._handle

    def connect(
        self,
        *,
        user: str = "postgamma",
        database: str = "postgres",
        application_name: str = "postgamma-python",
        settings: Mapping[str, Any] | Iterable[tuple[str, Any]] | None = None,
        autocommit: bool = False,
        timeout: float | None = None,
    ) -> Connection:
        """Open one logical PostgreSQL session owned by this database.

        Args:
            user: Existing PostgreSQL role.  New roles are not created
                implicitly.
            database: Existing SQL database inside the cluster.  This is not
                the cluster-directory path.
            application_name: Value reported by PostgreSQL for this session.
            settings: Session-local PostgreSQL settings.
            autocommit: Commit every statement independently when true.  When
                false, ordinary statements begin an implicit transaction.
            timeout: Connection operation deadline in seconds.  ``None`` uses
                the database default rather than disabling the deadline.

        Returns:
            A logical connection.  Closing it leaves this shared database
            instance open.

        Raises:
            DatabaseError: PostgreSQL rejects the SQL database, role, or
                session options.
            OperationalError: The instance cannot start or accept the session.
        """

        with self._lock:
            instance = self._ensure_open()
            try:
                handle = _native.connection_open(
                    instance,
                    user=user,
                    database=database,
                    application_name=application_name,
                    settings=_normalize_settings(settings),
                )
            except _native.NativeError as error:
                raise_translated(error)
            connection = Connection(
                self,
                handle,
                autocommit=autocommit,
                timeout=(
                    self.timeout
                    if timeout is None
                    else _normalize_timeout(timeout)
                ),
                implicit_release=None,
            )
            self._connections.add(connection)
            return connection

    def execute(
        self,
        sql: str,
        parameters: Sequence[Any] | None = None,
        *,
        timeout: float | None = None,
    ) -> Result:
        """Execute one autocommitted statement on a temporary connection."""

        with self.connect(autocommit=True, timeout=timeout) as connection:
            return connection.execute(sql, parameters)

    def telemetry(self) -> dict[str, int]:
        """Return a synchronized executor, request, and event snapshot."""

        with self._lock:
            handle = self._ensure_open()
            try:
                return dict(_native.instance_telemetry(handle))
            except _native.NativeError as error:
                raise_translated(error)

    @property
    def waitable(self) -> int:
        """Return the level-triggered instance event descriptor."""

        with self._lock:
            handle = self._ensure_open()
            try:
                return int(_native.instance_waitable(handle))
            except _native.NativeError as error:
                raise_translated(error)

    def next_event(self, *, timeout: float | None = 0) -> Any | None:
        """Return the next owned log, notice, notification, or overflow event."""

        from ._management import event_from_native

        with self._lock:
            if self._event_backlog:
                return self._event_backlog.popleft()
            handle = self._ensure_open()
            try:
                native = _native.instance_next_event(
                    handle, timeout_ms=_timeout_milliseconds(timeout)
                )
            except _native.NativeError as error:
                raise_translated(error)
        return None if native is None else event_from_native(native)

    def add_event_handler(self, handler: Any, *, kind: int | None = None) -> None:
        """Register a host-thread event callback, optionally for one event kind."""

        if not callable(handler):
            raise TypeError("event handler must be callable")
        with self._lock:
            self._event_handlers.setdefault(kind, []).append(handler)

    def remove_event_handler(self, handler: Any, *, kind: int | None = None) -> None:
        """Remove a previously registered event callback."""

        with self._lock:
            handlers = self._event_handlers.get(kind, [])
            try:
                handlers.remove(handler)
            except ValueError:
                return
            if not handlers:
                self._event_handlers.pop(kind, None)

    def dispatch_events(self, *, maximum: int | None = None) -> int:
        """Drain queued events and invoke callbacks on the calling host thread."""

        if maximum is not None:
            maximum = _nonnegative_int(maximum, "maximum")
        dispatched = 0
        while maximum is None or dispatched < maximum:
            with self._lock:
                handle = self._ensure_open()
                try:
                    native = _native.instance_next_event(handle, timeout_ms=0)
                except _native.NativeError as error:
                    raise_translated(error)
                if native is None:
                    break
                from ._management import event_from_native

                event = event_from_native(native)
                handlers = tuple(self._event_handlers.get(None, ())) + tuple(
                    self._event_handlers.get(event.kind, ())
                )
                if event.kind == _native.EVENT_NOTICE:
                    handlers += tuple(
                        self._request_event_handlers.get(event.request_id, ())
                    )
                dispatched += 1
                if not handlers:
                    self._event_backlog.append(event)
                    continue
            for handler in handlers:
                handler(event)
        return dispatched

    def checkpoint(self, *, wait: bool = True, timeout: float | None = None) -> Any:
        """Start an in-process checkpoint and optionally wait for completion."""

        from ._management import Operation

        operation = Operation.checkpoint(self)
        return operation.wait(timeout=timeout) if wait else operation

    def maintenance(
        self,
        kind: Any = "vacuum_analyze",
        *,
        database: str | None = None,
        user: str | None = None,
        wait: bool = True,
        timeout: float | None = None,
    ) -> Any:
        """Run VACUUM, ANALYZE, VACUUM ANALYZE, or database REINDEX."""

        from ._management import Operation

        operation = Operation.maintenance(
            self, kind=kind, database_name=database, user=user
        )
        return operation.wait(timeout=timeout) if wait else operation

    def logical_dump(
        self,
        stream: Any,
        *,
        flags: int = 0,
        database: str | None = None,
        user: str | None = None,
        channel_capacity: int = 256 * 1024,
        progress_quantum: int = 64 * 1024,
        wait: bool = True,
        timeout: float | None = None,
    ) -> Any:
        """Write an in-process PostgreSQL logical archive to a binary stream."""

        from ._management import Operation

        operation = Operation.logical_dump(
            self,
            stream,
            flags=flags,
            database_name=database,
            user=user,
            channel_capacity=channel_capacity,
            progress_quantum=progress_quantum,
        )
        return operation.wait(timeout=timeout) if wait else operation

    def logical_restore(
        self,
        stream: Any,
        *,
        flags: int = 0,
        database: str | None = None,
        user: str | None = None,
        channel_capacity: int = 256 * 1024,
        progress_quantum: int = 64 * 1024,
        wait: bool = True,
        timeout: float | None = None,
    ) -> Any:
        """Restore an in-process PostgreSQL logical archive from a binary stream."""

        from ._management import Operation

        operation = Operation.logical_restore(
            self,
            stream,
            flags=flags,
            database_name=database,
            user=user,
            channel_capacity=channel_capacity,
            progress_quantum=progress_quantum,
        )
        return operation.wait(timeout=timeout) if wait else operation

    def bundled_extensions(self) -> tuple[Any, ...]:
        """Return metadata for extensions statically reviewed into this build."""

        from ._management import BundledExtension

        try:
            return tuple(
                BundledExtension.from_native(item)
                for item in _native.bundled_extensions()
            )
        except _native.NativeError as error:
            raise_translated(error)

    def as_async(self) -> Any:
        """Return an asyncio facade over this lazy database object."""

        from ._async import AsyncDatabase

        return AsyncDatabase(self)

    def close(self, *, mode: str = "fast", timeout: float | None = None) -> None:
        """Close the instance.

        A failed or timed-out close leaves the native handle open so the same
        ``Database`` may retry ``close()`` with another timeout or mode.
        """

        with self._lock:
            if self._closed:
                return
            for operation in list(self._operations):
                operation.close()
            for connection in list(self._connections):
                connection.close(timeout=timeout)
            if self._handle is not None:
                modes = {
                    "smart": _native.SHUTDOWN_SMART,
                    "fast": _native.SHUTDOWN_FAST,
                    "immediate": _native.SHUTDOWN_IMMEDIATE,
                }
                if mode not in modes:
                    raise ValueError("shutdown mode must be smart, fast, or immediate")
                try:
                    _native.instance_close(
                        self._handle,
                        mode=modes[mode],
                        timeout_ms=_timeout_milliseconds(
                            self.timeout if timeout is None else timeout
                        ),
                    )
                except _native.NativeError as error:
                    raise_translated(error)
                self._handle = None
            self._closed = True

    def close_before_fork(self, *, timeout: float | None = None) -> None:
        """Quiesce and close the instance before the host calls fork()."""

        self.close(mode="fast", timeout=timeout)

    def __enter__(self) -> Self:
        return self.open()

    def __exit__(self, exc_type: Any, exc: Any, traceback: Any) -> None:
        del traceback
        if exc_type is None:
            self.close()
            return
        try:
            self.close()
        except BaseException as cleanup_error:
            _warn_preserved_cleanup_error("database close", cleanup_error, exc)

    def __del__(self) -> None:
        if getattr(self, "_handle", None) is not None and not getattr(
            self, "_closed", True
        ):
            _warn_resource_leak("unclosed PostGamma Database", self)


class Connection:
    """One logical PostgreSQL session over the in-memory protocol.

    A connection preserves PostgreSQL transaction state, temporary objects,
    prepared statements, session settings, and notifications across calls.
    Operations on one connection are serialized; different connections owned
    by one database may execute concurrently.

    Connections returned by top-level :func:`postgamma.connect` lease a shared
    process-local ``Database`` for their canonical cluster path.  The last such
    connection closes the implicit instance.  A connection returned by
    :meth:`Database.connect` closes only its logical session.
    """

    def __init__(
        self,
        owner: Database,
        handle: Any,
        *,
        autocommit: bool,
        timeout: float | None,
        implicit_release: Any | None,
    ) -> None:
        self._database = owner
        self._handle = handle
        self._autocommit = bool(autocommit)
        self.timeout = timeout
        self._closed = False
        self._implicit_release = implicit_release
        self._transaction_scope_active = False
        self._operation_lock = threading.RLock()
        self._request_gate = threading.Lock()
        self._cursors: weakref.WeakSet[Cursor] = weakref.WeakSet()
        self._requests: weakref.WeakSet[Any] = weakref.WeakSet()
        self._statements: weakref.WeakSet[Any] = weakref.WeakSet()

    @property
    def closed(self) -> bool:
        """Return whether this logical connection is closed."""

        return self._closed

    @property
    def database(self) -> Database:
        """Return the database instance that owns this connection."""

        return self._database

    @property
    def autocommit(self) -> bool:
        """Return whether statements commit without an implicit transaction."""

        return self._autocommit

    @autocommit.setter
    def autocommit(self, value: bool) -> None:
        with self._operation_lock:
            self._require_open()
            if self.transaction_status not in {
                _native.TRANSACTION_IDLE,
            }:
                raise ProgrammingError(
                    "autocommit cannot change while a transaction is active"
                )
            self._autocommit = bool(value)

    @property
    def transaction_status(self) -> int:
        """Return the current PostgreSQL transaction-status constant."""

        self._require_open()
        try:
            return int(_native.connection_transaction_status(self._handle))
        except _native.NativeError as error:
            raise_translated(error)

    @property
    def id(self) -> int:
        """Return the stable PostGamma logical connection identity."""

        self._require_open()
        try:
            return int(_native.connection_identity(self._handle))
        except _native.NativeError as error:
            raise_translated(error)

    def parameter_status(self, name: str) -> str | None:
        """Return a PostgreSQL parameter-status value by name."""

        self._require_open()
        try:
            return _native.connection_parameter_status(self._handle, name)
        except _native.NativeError as error:
            raise_translated(error)

    @property
    def status(self) -> Any:
        """Return transaction, pinning, carrier, and request scheduling state."""

        from ._stream import ConnectionStatus

        self._require_open()
        try:
            return ConnectionStatus.from_native(_native.connection_status(self._handle))
        except _native.NativeError as error:
            raise_translated(error)

    def _require_open(self) -> None:
        if self._closed or self._handle is None:
            raise InterfaceError("PostGamma connection is closed")

    def _native_execute(
        self,
        sql: str,
        parameters: Sequence[Any] | None,
        timeout: float | None,
    ) -> Result:
        encoded = encode_parameters(parameters)
        with self._request_gate:
            try:
                native = _native.connection_execute(
                    self._handle,
                    sql,
                    encoded,
                    timeout_ms=_timeout_milliseconds(timeout),
                )
            except _native.NativeError as error:
                raise_translated(error)
        return Result(native)

    def execute(
        self,
        sql: str,
        parameters: Sequence[Any] | None = None,
        *,
        timeout: float | None = None,
    ) -> Result:
        """Execute one statement and return a materialized result.

        Use PostgreSQL extended-protocol markers such as ``$1`` and ``$2``.
        DB-API cursor markers such as ``:1`` apply only to :class:`Cursor`.

        Args:
            sql: One PostgreSQL statement.
            parameters: Values for positional markers, or ``None``.
            timeout: Statement deadline in seconds.  ``None`` uses the
                connection default.

        Returns:
            A Python-owned materialized result positioned before its first row.

        Raises:
            QueryTimeoutError: The host-side deadline expires.
            QueryCanceledError: This request is canceled.
            DatabaseError: PostgreSQL rejects or cannot execute the statement.
        """

        with self._operation_lock:
            self._require_open()
            effective_timeout = (
                self.timeout if timeout is None else _normalize_timeout(timeout)
            )
            if (
                not self._autocommit
                and not is_transaction_control(sql)
                and self.transaction_status == _native.TRANSACTION_IDLE
            ):
                self._native_execute("BEGIN", None, effective_timeout)
            return self._native_execute(sql, parameters, effective_timeout)

    def stream(
        self,
        sql: str,
        parameters: Sequence[Any] | None = None,
        *,
        chunk_rows: int = 1024,
        timeout: float | None = None,
        result_buffer_limit: int = 0,
        on_notice: Any | None = None,
    ) -> Any:
        """Execute a query as bounded, caller-driven row chunks."""

        from ._stream import RowStream

        with self._operation_lock:
            self._require_open()
            effective_timeout = (
                self.timeout if timeout is None else _normalize_timeout(timeout)
            )
            self._begin_transaction_if_needed(sql, effective_timeout)
            return RowStream.start(
                self,
                sql,
                parameters,
                chunk_rows=chunk_rows,
                timeout=effective_timeout,
                result_buffer_limit=result_buffer_limit,
                on_notice=on_notice,
            )

    def execute_script(
        self,
        sql: str,
        *,
        timeout: float | None = None,
    ) -> tuple[Result, ...]:
        """Execute simple-protocol SQL and return every ordered result."""

        from ._stream import Request

        with self._operation_lock:
            self._require_open()
            effective_timeout = (
                self.timeout if timeout is None else _normalize_timeout(timeout)
            )
            request = Request.start(
                self, sql, script=True, timeout=effective_timeout
            )
        primary_error: BaseException | None = None
        try:
            return tuple(request.results())
        except BaseException as error:
            primary_error = error
            raise
        finally:
            _cleanup_preserving("script request close", request.close, primary_error)

    def prepare(
        self,
        sql: str,
        *,
        parameter_type_oids: Sequence[int] | None = None,
    ) -> Any:
        """Create a reusable extended-protocol prepared statement."""

        from ._stream import PreparedStatement

        with self._operation_lock:
            self._require_open()
            with self._request_gate:
                return PreparedStatement.prepare(self, sql, parameter_type_oids)

    def copy(self, sql: str, *, timeout: float | None = None) -> Any:
        """Start COPY FROM STDIN or COPY TO STDOUT and return a byte-stream object."""

        from ._stream import start_copy

        with self._operation_lock:
            self._require_open()
            effective_timeout = (
                self.timeout if timeout is None else _normalize_timeout(timeout)
            )
            self._begin_transaction_if_needed(sql, effective_timeout)
            return start_copy(self, sql, timeout=effective_timeout)

    def copy_from(
        self,
        sql: str,
        source: Any,
        *,
        chunk_size: int = 64 * 1024,
        timeout: float | None = None,
    ) -> Result:
        """Copy bytes from a readable object and return PostgreSQL's command result."""

        transfer = self.copy(sql, timeout=timeout)
        primary_error: BaseException | None = None
        try:
            return transfer.copy_from(source, chunk_size=chunk_size)
        except BaseException as error:
            primary_error = error
            raise
        finally:
            _cleanup_preserving("COPY input close", transfer.close, primary_error)

    def copy_to(
        self,
        sql: str,
        target: Any,
        *,
        chunk_size: int = 64 * 1024,
        timeout: float | None = None,
    ) -> Result:
        """Copy bytes to a writable object and return PostgreSQL's command result."""

        transfer = self.copy(sql, timeout=timeout)
        primary_error: BaseException | None = None
        try:
            return transfer.copy_to(target, chunk_size=chunk_size)
        except BaseException as error:
            primary_error = error
            raise
        finally:
            _cleanup_preserving("COPY output close", transfer.close, primary_error)

    def execute_arrow(
        self,
        sql: str,
        parameters: Sequence[Any] | None = None,
        *,
        timeout: float | None = None,
    ) -> Any:
        """Execute a query and expose its struct array through Arrow C Data."""

        from ._arrow import ArrowResult

        with self._operation_lock:
            self._require_open()
            effective_timeout = (
                self.timeout if timeout is None else _normalize_timeout(timeout)
            )
            self._begin_transaction_if_needed(sql, effective_timeout)
            encoded = encode_parameters(parameters)
            with self._request_gate:
                try:
                    capsules = _native.connection_execute_arrow(
                        self._handle,
                        sql,
                        encoded,
                        timeout_ms=_timeout_milliseconds(effective_timeout),
                        result_format=_native.FORMAT_BINARY,
                    )
                except _native.NativeError as error:
                    raise_translated(error)
            return ArrowResult(capsules)

    def as_async(self) -> Any:
        """Return an asyncio facade over this connection."""

        from ._async import AsyncConnection

        return AsyncConnection(self)

    def _begin_transaction_if_needed(
        self, sql: str, timeout: float | None
    ) -> None:
        if (
            not self._autocommit
            and not is_transaction_control(sql)
            and self.transaction_status == _native.TRANSACTION_IDLE
        ):
            self._native_execute("BEGIN", None, timeout)

    def cursor(self) -> Cursor:
        """Create a PEP 249 cursor owned by this connection."""

        self._require_open()
        cursor = Cursor(self)
        self._cursors.add(cursor)
        return cursor

    def transaction(self) -> _TransactionContext:
        """Return a transaction scope that commits or rolls back without closing.

        The connection must be idle when the scope is entered.  Nested scopes
        are rejected rather than silently changing transaction ownership.
        """

        return _TransactionContext(self)

    def _claim_transaction_scope(self) -> None:
        with self._operation_lock:
            self._require_open()
            if self._transaction_scope_active:
                raise ProgrammingError("nested transaction scopes are not supported")
            if self.transaction_status != _native.TRANSACTION_IDLE:
                raise ProgrammingError(
                    "transaction() requires an idle connection; commit or roll "
                    "back the active transaction first"
                )
            self._transaction_scope_active = True

    def _enter_transaction_scope(self) -> None:
        with self._operation_lock:
            self._claim_transaction_scope()
            try:
                self._native_execute("BEGIN", None, self.timeout)
            except BaseException:
                self._transaction_scope_active = False
                raise

    def _release_transaction_scope(self) -> None:
        with self._operation_lock:
            self._transaction_scope_active = False

    def _complete_transaction_scope(self, *, commit: bool) -> None:
        with self._operation_lock:
            self._require_open()
            if self.transaction_status != _native.TRANSACTION_IDLE:
                self._native_execute(
                    "COMMIT" if commit else "ROLLBACK", None, self.timeout
                )

    def commit(self) -> None:
        """Commit the active transaction, if any."""

        with self._operation_lock:
            self._require_open()
            if self._transaction_scope_active:
                raise ProgrammingError(
                    "transaction() owns commit; leave its context to commit"
                )
            if self.transaction_status != _native.TRANSACTION_IDLE:
                self._native_execute("COMMIT", None, self.timeout)

    def rollback(self) -> None:
        """Roll back the active transaction, if any."""

        with self._operation_lock:
            self._require_open()
            if self._transaction_scope_active:
                raise ProgrammingError(
                    "transaction() owns rollback; raise from its context to roll back"
                )
            if self.transaction_status != _native.TRANSACTION_IDLE:
                self._native_execute("ROLLBACK", None, self.timeout)

    def cancel(self) -> bool:
        """Request cancellation of the active operation, if one exists."""

        handle = self._handle
        if self._closed or handle is None:
            raise InterfaceError("PostGamma connection is closed")
        try:
            return bool(_native.connection_cancel(handle))
        except _native.NativeError as error:
            raise_translated(error)

    def close(self, *, timeout: float | None = None) -> None:
        """Close this session and its cursors.

        A failed or timed-out close preserves the native handle and leaves the
        connection open, allowing the caller to retry ``close()``.
        """

        with self._operation_lock:
            if self._closed:
                return
            if self._transaction_scope_active:
                raise ProgrammingError(
                    "cannot close a connection inside its transaction() scope"
                )
            for cursor in list(self._cursors):
                cursor.close()
            for request in list(self._requests):
                request.close()
            for statement in list(self._statements):
                statement.close()
            try:
                if self.transaction_status != _native.TRANSACTION_IDLE:
                    self._native_execute("ROLLBACK", None, self.timeout)
                _native.connection_close(
                    self._handle,
                    timeout_ms=_timeout_milliseconds(
                        self.timeout if timeout is None else timeout
                    ),
                )
            except _native.NativeError as error:
                raise_translated(error)
            self._handle = None
            self._closed = True
            release = self._implicit_release
            self._implicit_release = None
        if release is not None:
            release(timeout)

    def __enter__(self) -> Self:
        self._require_open()
        return self

    def __exit__(self, exc_type: Any, exc: Any, traceback: Any) -> None:
        del traceback
        if exc_type is None:
            try:
                self.commit()
            except BaseException as primary_error:
                try:
                    self.close()
                except BaseException as cleanup_error:
                    _warn_preserved_cleanup_error(
                        "connection close", cleanup_error, primary_error
                    )
                raise
            self.close()
            return
        try:
            self.rollback()
        except BaseException as cleanup_error:
            _warn_preserved_cleanup_error("transaction rollback", cleanup_error, exc)
        try:
            self.close()
        except BaseException as cleanup_error:
            _warn_preserved_cleanup_error("connection close", cleanup_error, exc)

    def __del__(self) -> None:
        if getattr(self, "_handle", None) is not None and not getattr(
            self, "_closed", True
        ):
            _warn_resource_leak("unclosed PostGamma Connection", self)
            try:
                self.close()
            except BaseException as cleanup_error:
                _warn_preserved_cleanup_error(
                    "abandoned connection close", cleanup_error, None
                )


class _TransactionContext:
    """Single-use synchronous transaction scope for one logical connection."""

    def __init__(self, connection: Connection) -> None:
        self._connection = connection
        self._entered = False
        self._used = False

    def __enter__(self) -> Connection:
        if self._used:
            raise ProgrammingError("a transaction scope is single-use")
        self._used = True
        self._connection._enter_transaction_scope()
        self._entered = True
        return self._connection

    def __exit__(self, exc_type: Any, exc: Any, traceback: Any) -> None:
        del traceback
        if not self._entered:
            return
        try:
            if exc_type is None:
                self._connection._complete_transaction_scope(commit=True)
            else:
                try:
                    self._connection._complete_transaction_scope(commit=False)
                except BaseException as cleanup_error:
                    _warn_preserved_cleanup_error(
                        "transaction-scope rollback", cleanup_error, exc
                    )
        finally:
            self._connection._release_transaction_scope()
            self._entered = False


class Cursor:
    """PEP 249 cursor backed by materialized PostGamma results."""

    def __init__(self, connection: Connection) -> None:
        self.connection = connection
        self.arraysize = 1
        self._result: Result | None = None
        self._closed = False
        self.rowcount = -1
        self.lastrowid = None
        self.description: tuple[tuple[Any, ...], ...] | None = None

    @property
    def closed(self) -> bool:
        """Return whether this cursor is closed."""

        return self._closed

    def _require_open(self) -> None:
        if self._closed:
            raise InterfaceError("PostGamma cursor is closed")
        self.connection._require_open()

    def execute(
        self,
        operation: str,
        parameters: Sequence[Any] | None = None,
        *,
        timeout: float | None = None,
    ) -> Self:
        """Execute one numeric-parameter DB-API statement and return this cursor."""

        self._require_open()
        try:
            adapted = rewrite_numeric_parameters(operation)
        except (TypeError, ValueError) as error:
            raise ProgrammingError(str(error)) from error
        self._result = self.connection.execute(adapted, parameters, timeout=timeout)
        self.rowcount = self._result.rowcount
        self.description = self._result.description
        return self

    def executemany(
        self,
        operation: str,
        seq_of_parameters: Iterable[Sequence[Any]],
    ) -> Self:
        """Execute one statement for each supplied parameter sequence."""

        self._require_open()
        total = 0
        executed = False
        for parameters in seq_of_parameters:
            self.execute(operation, parameters)
            executed = True
            if self.rowcount >= 0:
                total += self.rowcount
        if not executed:
            self._result = None
            self.description = None
        self.rowcount = total if executed else 0
        return self

    def callproc(
        self, procedure_name: str, parameters: Sequence[Any] | None = None
    ) -> None:
        """Raise ``NotSupportedError`` because procedure calls are unavailable."""

        self._require_open()
        del procedure_name, parameters
        raise NotSupportedError(
            "stored-procedure calls are not exposed by the PostGamma Python API"
        )

    def fetchone(self) -> tuple[Any, ...] | None:
        """Return the next tuple row, or ``None`` when exhausted."""

        self._require_open()
        if self._result is None or self.description is None:
            raise ProgrammingError("the cursor has no tuple result")
        return self._result.fetchone()

    def fetchmany(self, size: int | None = None) -> list[tuple[Any, ...]]:
        """Return up to ``size`` rows, defaulting to ``arraysize``."""

        self._require_open()
        if self._result is None or self.description is None:
            raise ProgrammingError("the cursor has no tuple result")
        return self._result.fetchmany(self.arraysize if size is None else size)

    def fetchall(self) -> list[tuple[Any, ...]]:
        """Return every remaining tuple row."""

        self._require_open()
        if self._result is None or self.description is None:
            raise ProgrammingError("the cursor has no tuple result")
        return self._result.fetchall()

    def close(self) -> None:
        """Close this cursor without closing its connection."""

        self._result = None
        self._closed = True

    def nextset(self) -> None:
        """Return ``None`` because the cursor has no later result set."""

        self._require_open()
        return None

    def setinputsizes(self, sizes: Any) -> None:
        """Accept the optional PEP 249 input-size hint as a no-op."""

        self._require_open()
        del sizes

    def setoutputsize(self, size: Any, column: Any = None) -> None:
        """Accept the optional PEP 249 output-size hint as a no-op."""

        self._require_open()
        del size, column

    def __iter__(self) -> Self:
        self._require_open()
        return self

    def __next__(self) -> tuple[Any, ...]:
        row = self.fetchone()
        if row is None:
            raise StopIteration
        return row

    def __enter__(self) -> Self:
        self._require_open()
        return self

    def __exit__(self, exc_type: Any, exc: Any, traceback: Any) -> None:
        self.close()


def _warn_preserved_cleanup_error(
    action: str, cleanup_error: BaseException, primary_error: BaseException | None
) -> None:
    primary = (
        f" while preserving {type(primary_error).__name__}"
        if primary_error is not None
        else ""
    )
    _emit_resource_warning(
        f"PostGamma {action} failed{primary}: {cleanup_error!r}", None
    )


def _cleanup_preserving(
    action: str, cleanup: Any, primary_error: BaseException | None
) -> None:
    try:
        cleanup()
    except BaseException as cleanup_error:
        if primary_error is None:
            raise
        _warn_preserved_cleanup_error(action, cleanup_error, primary_error)


def _warn_resource_leak(message: str, source: object) -> None:
    # Do not attach the leaking object as WarningMessage.source: a warning
    # recorder could otherwise resurrect the object while its finalizer runs.
    del source
    _emit_resource_warning(message, None)


def _emit_resource_warning(message: str, source: object | None) -> None:
    try:
        warnings.warn(message, ResourceWarning, stacklevel=3, source=source)
    except BaseException:
        # Warning filters may promote ResourceWarning to an exception.  A
        # destructor or secondary cleanup failure must never replace the
        # application's primary exception.
        pass


@dataclass
class _ImplicitDatabaseEntry:
    key: tuple[int, str]
    database: Database
    configuration: tuple[tuple[str, Any], ...]
    leases: int = 0
    closing: bool = False


_IMPLICIT_DATABASES: dict[tuple[int, str], _ImplicitDatabaseEntry] = {}
_IMPLICIT_DATABASES_CONDITION = threading.Condition(threading.RLock())


def _canonical_cluster_path(path: str | os.PathLike[str]) -> str:
    value = os.fspath(path)
    if not isinstance(value, str):
        raise TypeError("path must resolve to a string filesystem path")
    if not value:
        raise ValueError("path must not be empty")
    return os.path.realpath(os.path.abspath(value))


def _canonical_optional_path(path: str | os.PathLike[str] | None) -> str | None:
    return None if path is None else _canonical_cluster_path(path)


def _implicit_configuration(
    *,
    settings: tuple[tuple[str, str], ...],
    worker_count: int,
    resource_root: str | os.PathLike[str] | None,
    executable_path: str | os.PathLike[str] | None,
) -> tuple[tuple[str, Any], ...]:
    return (
        ("settings", settings),
        ("worker_count", worker_count),
        ("resource_root", _canonical_optional_path(resource_root)),
        ("executable_path", _canonical_optional_path(executable_path)),
    )


def _acquire_implicit_database(
    path: str | os.PathLike[str],
    *,
    mode: OpenMode,
    settings: tuple[tuple[str, str], ...],
    worker_count: int,
    timeout: float | None,
    resource_root: str | os.PathLike[str] | None,
    executable_path: str | os.PathLike[str] | None,
) -> _ImplicitDatabaseEntry:
    canonical_path = _canonical_cluster_path(path)
    key = (os.getpid(), canonical_path)
    configuration = _implicit_configuration(
        settings=settings,
        worker_count=worker_count,
        resource_root=resource_root,
        executable_path=executable_path,
    )
    with _IMPLICIT_DATABASES_CONDITION:
        entry = _IMPLICIT_DATABASES.get(key)
        while entry is not None and entry.closing:
            _IMPLICIT_DATABASES_CONDITION.wait()
            entry = _IMPLICIT_DATABASES.get(key)
        if entry is not None and entry.database.closed:
            _IMPLICIT_DATABASES.pop(key, None)
            entry = None
        if entry is not None:
            if mode is OpenMode.CREATE_NEW:
                raise OperationalError(
                    f"cluster path already has a live embedded instance: "
                    f"{canonical_path}",
                    status_name="already-exists",
                )
            current = dict(entry.configuration)
            requested = dict(configuration)
            conflicts = sorted(
                name for name in current if current[name] != requested[name]
            )
            if conflicts:
                raise InterfaceError(
                    "implicit instance options conflict for "
                    f"{canonical_path}: {', '.join(conflicts)}"
                )
        else:
            database = Database(
                canonical_path,
                mode=mode,
                settings=settings,
                worker_count=worker_count,
                timeout=timeout,
                resource_root=resource_root,
                executable_path=executable_path,
            )
            entry = _ImplicitDatabaseEntry(key, database, configuration)
            _IMPLICIT_DATABASES[key] = entry
        entry.leases += 1
        return entry


def _release_implicit_database(
    entry: _ImplicitDatabaseEntry, timeout: float | None
) -> None:
    with _IMPLICIT_DATABASES_CONDITION:
        if entry.leases <= 0:
            raise RuntimeError("implicit database lease was released twice")
        entry.leases -= 1
        if entry.leases != 0:
            return
        entry.closing = True
    close_error: BaseException | None = None
    try:
        entry.database.close(timeout=timeout)
    except BaseException as error:
        close_error = error
    finally:
        with _IMPLICIT_DATABASES_CONDITION:
            if close_error is None and _IMPLICIT_DATABASES.get(entry.key) is entry:
                _IMPLICIT_DATABASES.pop(entry.key, None)
            entry.closing = False
            _IMPLICIT_DATABASES_CONDITION.notify_all()
    if close_error is not None:
        raise close_error


def _reset_implicit_registry_lock_after_fork() -> None:
    global _IMPLICIT_DATABASES_CONDITION

    _IMPLICIT_DATABASES_CONDITION = threading.Condition(threading.RLock())


if hasattr(os, "register_at_fork"):
    os.register_at_fork(after_in_child=_reset_implicit_registry_lock_after_fork)


def connect(
    path: str | os.PathLike[str],
    *,
    mode: OpenMode | str = OpenMode.OPEN_OR_CREATE,
    database: str = "postgres",
    user: str = "postgamma",
    application_name: str = "postgamma-python",
    settings: Mapping[str, Any] | Iterable[tuple[str, Any]] | None = None,
    connection_settings: Mapping[str, Any] | Iterable[tuple[str, Any]] | None = None,
    worker_count: int = 4,
    autocommit: bool = False,
    timeout: float | None = _DEFAULT_TIMEOUT_SECONDS,
    resource_root: str | os.PathLike[str] | None = None,
    executable_path: str | os.PathLike[str] | None = None,
) -> Connection:
    """Open a logical session on a shared process-local embedded instance.

    ``path`` is a PostgreSQL cluster directory.  The default mode opens an
    existing compatible cluster or initializes a missing final directory.  All
    top-level connections for the same canonical path share one embedded
    instance while retaining independent PostgreSQL session and transaction
    state.

    Args:
        path: Persistent PostgreSQL cluster-directory path.
        mode: ``open_or_create``, ``open_existing``, or ``create_new``.
        database: Existing PostgreSQL logical database inside the cluster.
        user: Existing PostgreSQL role for the logical session.
        application_name: PostgreSQL application name for the session.
        settings: Instance/startup PostgreSQL settings.  Repeated top-level
            connections must request the same instance settings.
        connection_settings: Session-local PostgreSQL settings.
        worker_count: Carrier workers shared by sessions on this instance.
        autocommit: Commit each statement independently when true.
        timeout: Default operation deadline in seconds, or ``None`` for no
            deadline.
        resource_root: Advanced resource-pack override; wheels resolve it.
        executable_path: Advanced PostgreSQL executable-identity override;
            wheels resolve it to ``bin/postgres`` in their resource pack and
            do not launch it.

    Returns:
        An independent logical connection.  Its close releases one instance
        lease; the last implicit connection shuts down the shared instance.

    Raises:
        OperationalError: The path violates the requested mode or cannot open.
        InterfaceError: Instance options conflict with a live implicit instance.
        DatabaseError: PostgreSQL rejects the requested logical database or role.

    Example:
        >>> import postgamma
        >>> with postgamma.connect("application.pgm", autocommit=True) as connection:
        ...     connection.execute("create table if not exists values_(n int)")
    """

    normalized_mode = OpenMode.parse(mode)
    normalized_settings = _normalize_settings(settings)
    normalized_worker_count = _positive_int(worker_count, "worker_count")
    normalized_timeout = _normalize_timeout(timeout)
    resolved_resource_root, resolved_executable_path = _resolve_runtime_paths(
        resource_root, executable_path
    )
    entry = _acquire_implicit_database(
        path,
        mode=normalized_mode,
        settings=normalized_settings,
        worker_count=normalized_worker_count,
        timeout=normalized_timeout,
        resource_root=resolved_resource_root,
        executable_path=resolved_executable_path,
    )
    try:
        connection = entry.database.connect(
            user=user,
            database=database,
            application_name=application_name,
            settings=connection_settings,
            autocommit=autocommit,
            timeout=normalized_timeout,
        )
    except BaseException as primary_error:
        try:
            _release_implicit_database(entry, normalized_timeout)
        except BaseException as cleanup_error:
            _warn_preserved_cleanup_error(
                "implicit database release", cleanup_error, primary_error
            )
        raise

    def release(close_timeout: float | None) -> None:
        _release_implicit_database(entry, close_timeout)

    connection._implicit_release = release
    return connection


def _normalize_settings(
    settings: Mapping[str, Any] | Iterable[tuple[str, Any]] | None,
) -> tuple[tuple[str, str], ...]:
    if settings is None:
        return ()
    items = settings.items() if isinstance(settings, Mapping) else settings
    normalized: list[tuple[str, str]] = []
    for name, value in items:
        if not isinstance(name, str) or not name:
            raise TypeError("setting names must be non-empty strings")
        normalized.append((name, str(value)))
    return tuple(normalized)


def _positive_int(value: int, name: str) -> int:
    if not isinstance(value, int) or isinstance(value, bool) or value <= 0:
        raise ValueError(f"{name} must be a positive integer")
    return value


def _nonnegative_int(value: int, name: str) -> int:
    if not isinstance(value, int) or isinstance(value, bool) or value < 0:
        raise ValueError(f"{name} must be a non-negative integer")
    return value


def _normalize_timeout(timeout: float | None) -> float | None:
    if timeout is None:
        return None
    value = float(timeout)
    if not math.isfinite(value) or value < 0:
        raise ValueError("timeout must be a finite non-negative value")
    return value


def _timeout_milliseconds(timeout: float | None) -> int:
    if timeout is None:
        return -1
    value = _normalize_timeout(timeout)
    assert value is not None
    milliseconds = math.ceil(value * 1000)
    if milliseconds > (1 << 63) - 1:
        raise OverflowError("timeout exceeds the native int64 range")
    return milliseconds


def _resolve_runtime_paths(
    resource_root: str | os.PathLike[str] | None,
    executable_path: str | os.PathLike[str] | None,
) -> tuple[str, str]:
    root_value = resource_root or os.environ.get("POSTGAMMA_RESOURCE_ROOT")
    if root_value is None:
        packaged = Path(__file__).resolve().parent / "_resources"
        if packaged.is_dir():
            root_value = os.fspath(packaged)
    if root_value is None:
        raise InterfaceError(
            "PostGamma runtime resources are unavailable; install a binary wheel "
            "or set POSTGAMMA_RESOURCE_ROOT"
        )
    root = Path(root_value).resolve()
    executable = (
        Path(executable_path).resolve()
        if executable_path is not None
        else root / "bin" / "postgres"
    )
    if not (root / "share" / "postgres.bki").is_file():
        raise InterfaceError(f"invalid PostGamma resource root: {root}")
    if not executable.is_file():
        raise InterfaceError(f"PostGamma executable identity is missing: {executable}")
    return os.fspath(root), os.fspath(executable)
