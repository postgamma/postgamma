"""PEP 249 exceptions and native diagnostic translation."""

from __future__ import annotations

from typing import NoReturn

from . import _native


class Warning(Exception):
    """A non-fatal database warning."""


class Error(Exception):
    """Base class for every PostGamma database exception."""

    def __init__(
        self,
        message: str,
        *,
        status: int | None = None,
        status_name: str | None = None,
        sqlstate: str | None = None,
        severity: str | None = None,
        detail: str | None = None,
        hint: str | None = None,
    ) -> None:
        super().__init__(message)
        self.status = status
        self.status_name = status_name
        self.sqlstate = sqlstate
        self.severity = severity
        self.detail = detail
        self.hint = hint


class InterfaceError(Error):
    """The host-facing API or process lifecycle contract was violated."""


class DatabaseError(Error):
    """Base class for errors reported by PostgreSQL."""


class DataError(DatabaseError):
    """A value could not be processed."""


class OperationalError(DatabaseError):
    """The database operation failed for an operational reason."""


class IntegrityError(DatabaseError):
    """A relational integrity constraint was violated."""


class InternalError(DatabaseError):
    """The database reported an internal failure."""


class ProgrammingError(DatabaseError):
    """SQL or API usage was invalid."""


class NotSupportedError(DatabaseError):
    """The requested capability is not implemented by this build."""


class QueryCanceledError(OperationalError):
    """A running request was canceled."""


class QueryTimeoutError(OperationalError):
    """A request exceeded its host-side deadline."""


class ForkedProcessError(InterfaceError):
    """An inherited handle was used in a child created with fork()."""


def _exception_class(error: _native.NativeError) -> type[Error]:
    status = getattr(error, "status", None)
    sqlstate = getattr(error, "sqlstate", None)
    sqlstate_class = sqlstate[:2] if isinstance(sqlstate, str) else None

    if status == _native.PGM_STATUS_FORKED_PROCESS:
        return ForkedProcessError
    if status == _native.PGM_STATUS_UNSUPPORTED:
        return NotSupportedError
    if status == _native.PGM_STATUS_TIMEOUT:
        return QueryTimeoutError
    if status == _native.PGM_STATUS_CANCELED or sqlstate == "57014":
        return QueryCanceledError
    if status in {
        _native.PGM_STATUS_INVALID_ARGUMENT,
        _native.PGM_STATUS_VERSION_MISMATCH,
    }:
        return InterfaceError
    if status == _native.PGM_STATUS_BUSY:
        return OperationalError
    if status in {
        _native.PGM_STATUS_IO_ERROR,
        _native.PGM_STATUS_CONNECTION_FAILED,
        _native.PGM_STATUS_INSTANCE_FAILED,
        _native.PGM_STATUS_OUT_OF_MEMORY,
    }:
        return OperationalError
    if status in {
        _native.PGM_STATUS_INTERNAL_ERROR,
        _native.PGM_STATUS_REENTRANT_CALL,
    }:
        return InternalError
    # PGM_STATUS_POSTGRES_ERROR deliberately falls through to SQLSTATE-based
    # classification so server diagnostics retain their DB-API specificity.
    if sqlstate_class == "22":
        return DataError
    if sqlstate_class == "23":
        return IntegrityError
    if sqlstate_class in {"08", "40", "53", "54", "55", "57", "58"}:
        return OperationalError
    if sqlstate_class in {"0A"}:
        return NotSupportedError
    if sqlstate_class in {"2D", "2F", "34", "3D", "3F", "42"}:
        return ProgrammingError
    if sqlstate_class == "XX":
        return InternalError
    return DatabaseError


def raise_translated(error: _native.NativeError) -> NoReturn:
    """Raise a stable public exception while preserving the native cause."""

    exception_type = _exception_class(error)
    public = exception_type(
        str(error),
        status=getattr(error, "status", None),
        status_name=getattr(error, "status_name", None),
        sqlstate=getattr(error, "sqlstate", None),
        severity=getattr(error, "severity", None),
        detail=getattr(error, "detail", None),
        hint=getattr(error, "hint", None),
    )
    raise public from error
