"""Instance events and caller-driven management operations."""

from __future__ import annotations

import select
import time
from dataclasses import dataclass
from enum import IntEnum, IntFlag
from typing import Any

from . import _native
from ._api import (
    Database,
    _cleanup_preserving,
    _normalize_timeout,
    _positive_int,
    _warn_resource_leak,
)
from ._errors import (
    InterfaceError,
    QueryCanceledError,
    QueryTimeoutError,
    raise_translated,
)


_POLL_SECONDS = 0.05


class EventKind(IntEnum):
    """Kinds delivered through an instance event queue."""

    LOG = _native.EVENT_LOG
    NOTICE = _native.EVENT_NOTICE
    NOTIFICATION = _native.EVENT_NOTIFICATION
    OVERFLOW = _native.EVENT_OVERFLOW


@dataclass(frozen=True, slots=True)
class Event:
    """Common identity fields carried by every owned instance event."""

    kind: EventKind
    connection_id: int
    request_id: int


@dataclass(frozen=True, slots=True)
class LogEvent(Event):
    """One structured PostgreSQL log record routed to the host."""

    virtual_backend_pid: int
    severity: str
    sqlstate: str
    message: str
    detail: str


@dataclass(frozen=True, slots=True)
class NoticeEvent(Event):
    """One structured PostgreSQL notice associated with a request."""

    severity: str
    sqlstate: str
    message: str
    detail: str
    hint: str


@dataclass(frozen=True, slots=True)
class NotificationEvent(Event):
    """One LISTEN/NOTIFY message routed to its logical connection."""

    virtual_backend_pid: int
    channel: str
    payload: str


@dataclass(frozen=True, slots=True)
class OverflowEvent(Event):
    """An observable count of events dropped by the bounded queue."""

    dropped_kind: EventKind
    dropped_count: int


def event_from_native(value: dict[str, Any]) -> Event:
    kind = EventKind(int(value["kind"]))
    common = (kind, int(value["connection_id"]), int(value["request_id"]))
    if kind is EventKind.LOG:
        return LogEvent(
            *common,
            int(value["virtual_backend_pid"]),
            str(value["severity"]),
            str(value["sqlstate"]),
            str(value["message"]),
            str(value["detail"]),
        )
    if kind is EventKind.NOTICE:
        return NoticeEvent(
            *common,
            str(value["severity"]),
            str(value["sqlstate"]),
            str(value["message"]),
            str(value["detail"]),
            str(value["hint"]),
        )
    if kind is EventKind.NOTIFICATION:
        return NotificationEvent(
            *common,
            int(value["virtual_backend_pid"]),
            str(value["channel"]),
            str(value["payload"]),
        )
    return OverflowEvent(
        *common,
        EventKind(int(value["dropped_kind"])),
        int(value["dropped_count"]),
    )


class OperationState(IntEnum):
    """Lifecycle states for a caller-driven management operation."""

    PENDING = _native.OPERATION_PENDING
    RUNNING = _native.OPERATION_RUNNING
    COMPLETED = _native.OPERATION_COMPLETED
    CANCELED = _native.OPERATION_CANCELED
    FAILED = _native.OPERATION_FAILED


class OperationKind(IntEnum):
    """Kinds of management operations supported by the current kernel."""

    CHECKPOINT = _native.OPERATION_KIND_CHECKPOINT
    LOGICAL_DUMP = _native.OPERATION_KIND_LOGICAL_DUMP
    LOGICAL_RESTORE = _native.OPERATION_KIND_LOGICAL_RESTORE
    MAINTENANCE = _native.OPERATION_KIND_MAINTENANCE


class OperationPhase(IntEnum):
    """Stable progress phases shared by management operations."""

    PENDING = _native.OPERATION_PHASE_PENDING
    STARTING = _native.OPERATION_PHASE_STARTING
    DATABASE = _native.OPERATION_PHASE_DATABASE
    ARCHIVE_IO = _native.OPERATION_PHASE_ARCHIVE_IO
    HOST_IO = _native.OPERATION_PHASE_HOST_IO
    FINALIZING = _native.OPERATION_PHASE_FINALIZING
    TERMINAL = _native.OPERATION_PHASE_TERMINAL


class MaintenanceKind(IntEnum):
    """Supported in-process PostgreSQL maintenance commands."""

    VACUUM = _native.MAINTENANCE_VACUUM
    ANALYZE = _native.MAINTENANCE_ANALYZE
    VACUUM_ANALYZE = _native.MAINTENANCE_VACUUM_ANALYZE
    REINDEX_DATABASE = _native.MAINTENANCE_REINDEX_DATABASE

    @classmethod
    def parse(cls, value: MaintenanceKind | str | int) -> MaintenanceKind:
        """Normalize a name, integer, or existing enum to a maintenance kind."""

        if isinstance(value, cls):
            return value
        if isinstance(value, str):
            normalized = value.strip().lower().replace(" ", "_")
            choices = {
                "vacuum": cls.VACUUM,
                "analyze": cls.ANALYZE,
                "vacuum_analyze": cls.VACUUM_ANALYZE,
                "reindex": cls.REINDEX_DATABASE,
                "reindex_database": cls.REINDEX_DATABASE,
            }
            try:
                return choices[normalized]
            except KeyError as error:
                raise ValueError(f"unknown maintenance kind: {value}") from error
        return cls(value)


class LogicalFlags(IntFlag):
    """Composable logical dump and restore options."""

    NONE = 0
    SCHEMA_ONLY = _native.LOGICAL_SCHEMA_ONLY
    DATA_ONLY = _native.LOGICAL_DATA_ONLY
    CLEAN = _native.LOGICAL_CLEAN
    CREATE = _native.LOGICAL_CREATE
    NO_OWNER = _native.LOGICAL_NO_OWNER
    NO_PRIVILEGES = _native.LOGICAL_NO_PRIVILEGES


@dataclass(frozen=True, slots=True)
class OperationProgress:
    """A synchronized management-operation progress snapshot."""

    kind: OperationKind
    state: OperationState
    phase: OperationPhase
    bytes_received: int
    bytes_produced: int
    total_bytes: int
    objects_completed: int
    objects_total: int

    @classmethod
    def from_native(cls, value: dict[str, Any]) -> OperationProgress:
        """Construct a typed progress snapshot from the native ABI mapping."""

        return cls(
            OperationKind(int(value["kind"])),
            OperationState(int(value["state"])),
            OperationPhase(int(value["phase"])),
            int(value["bytes_received"]),
            int(value["bytes_produced"]),
            int(value["total_bytes"]),
            int(value["objects_completed"]),
            int(value["objects_total"]),
        )


@dataclass(frozen=True, slots=True)
class BundledExtension:
    """Borrowed build metadata copied for one reviewed bundled extension."""

    id: str
    sql_name: str
    version: str
    postgresql_major: int
    sdk_abi_version: int
    capabilities: int

    @classmethod
    def from_native(cls, value: dict[str, Any]) -> BundledExtension:
        """Copy one native bundled-extension metadata mapping."""

        return cls(
            str(value["id"]),
            str(value["sql_name"]),
            str(value["version"]),
            int(value["postgresql_major"]),
            int(value["sdk_abi_version"]),
            int(value["capabilities"]),
        )


class Operation:
    """One in-process checkpoint, maintenance, dump, or restore operation."""

    __slots__ = ("database", "_handle", "_closed", "waitable", "__weakref__")

    def __init__(self, database: Database, handle: Any) -> None:
        self.database = database
        self._handle = handle
        self._closed = False
        try:
            self.waitable = int(_native.operation_waitable(handle))
        except _native.NativeError as error:
            try:
                _native.operation_close(handle)
            finally:
                raise_translated(error)
        database._operations.add(self)

    @classmethod
    def checkpoint(cls, database: Database) -> Operation:
        """Create a pending checkpoint operation for a database."""

        with database._lock:
            instance = database._ensure_open()
            try:
                handle = _native.operation_checkpoint(instance)
            except _native.NativeError as error:
                raise_translated(error)
        return cls(database, handle)

    @classmethod
    def maintenance(
        cls,
        owner: Database,
        *,
        kind: MaintenanceKind | str | int,
        database_name: str | None = None,
        user: str | None = None,
    ) -> Operation:
        """Create a pending VACUUM, ANALYZE, or REINDEX operation."""

        with owner._lock:
            instance = owner._ensure_open()
            try:
                handle = _native.operation_maintenance(
                    instance,
                    int(MaintenanceKind.parse(kind)),
                    database=database_name or "postgres",
                    user=user or "postgamma",
                )
            except _native.NativeError as error:
                raise_translated(error)
        return cls(owner, handle)

    @classmethod
    def logical_dump(
        cls,
        database: Database,
        stream: Any,
        **options: Any,
    ) -> Operation:
        """Create a pending logical dump writing to a binary stream."""

        return cls._logical(database, stream, dump=True, **options)

    @classmethod
    def logical_restore(
        cls,
        database: Database,
        stream: Any,
        **options: Any,
    ) -> Operation:
        """Create a pending logical restore reading from a binary stream."""

        return cls._logical(database, stream, dump=False, **options)

    @classmethod
    def _logical(
        cls,
        owner: Database,
        stream: Any,
        *,
        dump: bool,
        flags: int,
        database_name: str | None = None,
        user: str | None,
        channel_capacity: int,
        progress_quantum: int,
    ) -> Operation:
        channel_capacity = _positive_int(channel_capacity, "channel_capacity")
        progress_quantum = _positive_int(progress_quantum, "progress_quantum")
        with owner._lock:
            instance = owner._ensure_open()
            function = (
                _native.operation_logical_dump
                if dump
                else _native.operation_logical_restore
            )
            try:
                handle = function(
                    instance,
                    stream,
                    flags=int(LogicalFlags(flags)),
                    database=database_name or "postgres",
                    user=user or "postgamma",
                    channel_capacity=channel_capacity,
                    progress_quantum=progress_quantum,
                )
            except _native.NativeError as error:
                raise_translated(error)
        return cls(owner, handle)

    def _require_open(self) -> None:
        if self._closed or self._handle is None:
            raise InterfaceError("PostGamma operation is closed")

    @property
    def snapshot(self) -> OperationProgress:
        """Return the most recent synchronized operation progress snapshot."""

        self._require_open()
        try:
            return OperationProgress.from_native(
                _native.operation_snapshot(self._handle)
            )
        except _native.NativeError as error:
            raise_translated(error)

    def progress(self) -> OperationState:
        """Submit or advance this operation once and return its state."""

        self._require_open()
        try:
            return OperationState(int(_native.operation_progress(self._handle)))
        except _native.NativeError as error:
            raise_translated(error)

    def wait(self, *, timeout: float | None = None) -> OperationProgress:
        """Drive this operation to a terminal state within a deadline."""

        normalized = (
            self.database.timeout if timeout is None else _normalize_timeout(timeout)
        )
        deadline = None if normalized is None else time.monotonic() + normalized
        primary_error: BaseException | None = None
        try:
            while True:
                state = self.progress()
                snapshot = self.snapshot
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
                    self.cancel()
                    raise QueryTimeoutError(
                        "PostGamma management operation exceeded its deadline",
                        status=_native.PGM_STATUS_TIMEOUT,
                        status_name="timeout",
                    )
                wait = _POLL_SECONDS if remaining is None else min(
                    _POLL_SECONDS, remaining
                )
                select.select([self.waitable], [], [], wait)
        except BaseException as error:
            primary_error = error
            raise
        finally:
            _cleanup_preserving(
                "management operation close", self.close, primary_error
            )

    def cancel(self) -> None:
        """Request cancellation when the operation kind permits it."""

        if self._closed or self._handle is None:
            return
        try:
            _native.operation_cancel(self._handle)
        except _native.NativeError as error:
            raise_translated(error)

    def close(self) -> None:
        """Retire and release this operation handle."""

        if self._closed:
            return
        try:
            _native.operation_close(self._handle)
        except _native.NativeError as error:
            raise_translated(error)
        self._handle = None
        self._closed = True
        self.database._operations.discard(self)

    def __await__(self) -> Any:
        from ._async import wait_operation

        return wait_operation(self).__await__()

    def __enter__(self) -> Operation:
        self._require_open()
        return self

    def __exit__(self, exc_type: Any, exc: Any, traceback: Any) -> None:
        del exc_type, exc, traceback
        self.close()

    def __del__(self) -> None:
        if getattr(self, "_handle", None) is not None and not getattr(
            self, "_closed", True
        ):
            _warn_resource_leak("unclosed PostGamma Operation", self)
