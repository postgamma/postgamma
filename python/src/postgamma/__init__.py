"""PostGamma: PostgreSQL 19 embedded directly in a Python process."""

from __future__ import annotations

from . import _native
from ._api import Column, Connection, Cursor, Database, OpenMode, Result, connect
from ._arrow import ArrowResult
from ._async import (
    AsyncConnection,
    AsyncCopy,
    AsyncDatabase,
    AsyncPreparedStatement,
    AsyncRowStream,
    connect_async,
)
from ._capabilities import Capability, PinReason
from ._codec import (
    Array,
    EncodedParameter,
    Json,
    RawValue,
    register_codec,
    unregister_codec,
)
from ._dbapi import (
    BINARY,
    DATETIME,
    NUMBER,
    ROWID,
    STRING,
    Binary,
    Date,
    DateFromTicks,
    Time,
    TimeFromTicks,
    Timestamp,
    TimestampFromTicks,
)
from ._errors import (
    DataError,
    DatabaseError,
    Error,
    ForkedProcessError,
    IntegrityError,
    InterfaceError,
    InternalError,
    NotSupportedError,
    OperationalError,
    ProgrammingError,
    QueryCanceledError,
    QueryTimeoutError,
    Warning,
)
from ._management import (
    BundledExtension,
    Event,
    EventKind,
    LogEvent,
    LogicalFlags,
    MaintenanceKind,
    NoticeEvent,
    NotificationEvent,
    Operation,
    OperationKind,
    OperationPhase,
    OperationProgress,
    OperationState,
    OverflowEvent,
)
from ._stream import (
    ConnectionStatus,
    CopyIn,
    CopyOut,
    PreparedStatement,
    Request,
    RowStream,
    StatementDescription,
)
from ._version import __version__


apilevel = "2.0"
threadsafety = 2
paramstyle = "numeric"


def library_info() -> dict[str, object]:
    """Return the loaded native library identity and capability mask."""

    return dict(_native.metadata())


def capabilities() -> Capability:
    """Return the optional capabilities advertised by the loaded kernel."""

    return Capability(int(_native.metadata()["capabilities"]))


__all__ = [
    "Array",
    "ArrowResult",
    "AsyncConnection",
    "AsyncCopy",
    "AsyncDatabase",
    "AsyncPreparedStatement",
    "AsyncRowStream",
    "BINARY",
    "Binary",
    "BundledExtension",
    "Capability",
    "Column",
    "Connection",
    "ConnectionStatus",
    "CopyIn",
    "CopyOut",
    "Cursor",
    "DataError",
    "DATETIME",
    "Database",
    "DatabaseError",
    "EncodedParameter",
    "Error",
    "Event",
    "EventKind",
    "ForkedProcessError",
    "IntegrityError",
    "InterfaceError",
    "InternalError",
    "Json",
    "LogEvent",
    "LogicalFlags",
    "MaintenanceKind",
    "NUMBER",
    "NotSupportedError",
    "NoticeEvent",
    "NotificationEvent",
    "Operation",
    "OperationKind",
    "OperationPhase",
    "OperationProgress",
    "OperationState",
    "OpenMode",
    "OperationalError",
    "ProgrammingError",
    "QueryCanceledError",
    "QueryTimeoutError",
    "ROWID",
    "RawValue",
    "Request",
    "Result",
    "RowStream",
    "STRING",
    "Date",
    "DateFromTicks",
    "Time",
    "TimeFromTicks",
    "Timestamp",
    "TimestampFromTicks",
    "PreparedStatement",
    "OverflowEvent",
    "PinReason",
    "StatementDescription",
    "Warning",
    "__version__",
    "apilevel",
    "connect",
    "connect_async",
    "capabilities",
    "library_info",
    "paramstyle",
    "register_codec",
    "threadsafety",
    "unregister_codec",
]
