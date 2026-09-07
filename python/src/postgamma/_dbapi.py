"""Required PEP 249 constructors and type objects."""

from __future__ import annotations

import datetime
import time
from collections.abc import Iterable
from typing import Any

from . import _codec


class DBAPITypeObject:
    """Compare a DB-API category with one PostgreSQL type OID."""

    __slots__ = ("_values",)

    def __init__(self, values: Iterable[int]) -> None:
        self._values = frozenset(values)

    def __eq__(self, other: Any) -> bool:
        return (
            isinstance(other, int)
            and not isinstance(other, bool)
            and other in self._values
        )

    __hash__ = None

    def __repr__(self) -> str:
        values = ", ".join(str(value) for value in sorted(self._values))
        return f"DBAPITypeObject({values})"


STRING = DBAPITypeObject((_codec.TEXT_OID, _codec.VARCHAR, 18, 19, 1042))
BINARY = DBAPITypeObject((_codec.BYTEA,))
NUMBER = DBAPITypeObject(
    (
        _codec.INT2,
        _codec.INT4,
        _codec.INT8,
        _codec.OID,
        _codec.FLOAT4,
        _codec.FLOAT8,
        _codec.NUMERIC,
    )
)
DATETIME = DBAPITypeObject(
    (
        _codec.DATE,
        _codec.TIME,
        _codec.TIMESTAMP,
        _codec.TIMESTAMPTZ,
        _codec.INTERVAL,
        1266,
    )
)
ROWID = DBAPITypeObject((_codec.OID,))


def Date(year: int, month: int, day: int) -> datetime.date:
    """Construct the date value required by PEP 249."""

    return datetime.date(year, month, day)


def Time(hour: int, minute: int, second: int) -> datetime.time:
    """Construct the time value required by PEP 249."""

    return datetime.time(hour, minute, second)


def Timestamp(
    year: int,
    month: int,
    day: int,
    hour: int,
    minute: int,
    second: int,
) -> datetime.datetime:
    """Construct the timestamp value required by PEP 249."""

    return datetime.datetime(year, month, day, hour, minute, second)


def DateFromTicks(ticks: float) -> datetime.date:
    """Construct a local date from seconds since the epoch."""

    fields = time.localtime(ticks)
    return Date(fields.tm_year, fields.tm_mon, fields.tm_mday)


def TimeFromTicks(ticks: float) -> datetime.time:
    """Construct a local time from seconds since the epoch."""

    fields = time.localtime(ticks)
    return Time(fields.tm_hour, fields.tm_min, fields.tm_sec)


def TimestampFromTicks(ticks: float) -> datetime.datetime:
    """Construct a local timestamp from seconds since the epoch."""

    fields = time.localtime(ticks)
    return Timestamp(
        fields.tm_year,
        fields.tm_mon,
        fields.tm_mday,
        fields.tm_hour,
        fields.tm_min,
        fields.tm_sec,
    )


def Binary(value: Any) -> bytes:
    """Convert a bytes-like value to the PEP 249 binary representation."""

    return bytes(value)
