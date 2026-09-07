"""Python/PostgreSQL value codecs for the first PostGamma Python surface."""

from __future__ import annotations

import datetime as _datetime
import json as _json
import math
import re
import threading
import uuid as _uuid
from dataclasses import dataclass
from decimal import Decimal
from typing import Any, Callable, Iterable, Mapping, Sequence


TEXT = 0
BINARY = 1

BOOL = 16
BYTEA = 17
INT8 = 20
INT2 = 21
INT4 = 23
TEXT_OID = 25
OID = 26
JSON = 114
FLOAT4 = 700
FLOAT8 = 701
VARCHAR = 1043
DATE = 1082
TIME = 1083
TIMESTAMP = 1114
TIMESTAMPTZ = 1184
INTERVAL = 1186
NUMERIC = 1700
UUID = 2950
JSONB = 3802


@dataclass(frozen=True, slots=True)
class RawValue:
    """A lossless value returned for an OID without a registered decoder."""

    oid: int
    format: int
    data: bytes

    def text(self, encoding: str = "utf-8") -> str:
        """Decode a text-format raw value with the requested encoding."""

        if self.format != TEXT:
            raise ValueError("binary RawValue does not have a text representation")
        return self.data.decode(encoding)


@dataclass(frozen=True, slots=True)
class Json:
    """Explicitly encode a Python value as jsonb."""

    value: Any


@dataclass(frozen=True, slots=True)
class Array:
    """Explicit PostgreSQL array value with an optional array type OID."""

    values: Sequence[Any]
    oid: int = 0


@dataclass(frozen=True, slots=True)
class EncodedParameter:
    """One explicit PostgreSQL parameter representation for a custom encoder."""

    oid: int
    format: int
    is_null: bool
    data: bytes

    def native(self) -> tuple[int, int, bool, bytes]:
        """Return the tuple consumed by the native parameter adapter."""

        return self.oid, self.format, self.is_null, self.data


Decoder = Callable[[bytes, int], Any]
Encoder = Callable[[Any], EncodedParameter]


_decoder_lock = threading.RLock()
_custom_decoders: dict[int, Decoder] = {}
_custom_encoders: dict[type[Any], Encoder] = {}


def register_codec(
    *,
    oid: int,
    decoder: Decoder,
    python_type: type[Any] | None = None,
    encoder: Encoder | None = None,
) -> None:
    """Register an application or bundled-extension codec.

    Registration is process-wide and affects values decoded after the call.
    A Python encoder is optional, but its type and callable must be supplied
    together.
    """

    if not isinstance(oid, int) or isinstance(oid, bool) or not 0 < oid <= 0xFFFFFFFF:
        raise ValueError("codec oid must be a positive uint32")
    if not callable(decoder):
        raise TypeError("decoder must be callable")
    if (python_type is None) != (encoder is None):
        raise ValueError("python_type and encoder must be supplied together")
    if python_type is not None and not isinstance(python_type, type):
        raise TypeError("python_type must be a type")
    with _decoder_lock:
        _custom_decoders[oid] = decoder
        if python_type is not None and encoder is not None:
            _custom_encoders[python_type] = encoder


def unregister_codec(oid: int, python_type: type[Any] | None = None) -> None:
    """Remove a previously registered custom codec."""

    with _decoder_lock:
        _custom_decoders.pop(oid, None)
        if python_type is not None:
            _custom_encoders.pop(python_type, None)


def _text(oid: int, value: str) -> EncodedParameter:
    return EncodedParameter(oid, TEXT, False, value.encode("utf-8"))


def _encode_interval(value: _datetime.timedelta) -> str:
    total_microseconds = (
        value.days * 86_400_000_000
        + value.seconds * 1_000_000
        + value.microseconds
    )
    sign = "-" if total_microseconds < 0 else ""
    total_microseconds = abs(total_microseconds)
    days, remainder = divmod(total_microseconds, 86_400_000_000)
    hours, remainder = divmod(remainder, 3_600_000_000)
    minutes, remainder = divmod(remainder, 60_000_000)
    seconds, microseconds = divmod(remainder, 1_000_000)
    fractional = f".{microseconds:06d}".rstrip("0") if microseconds else ""
    return f"{sign}{days} days {hours:02d}:{minutes:02d}:{seconds:02d}{fractional}"


def _array_quote(value: str) -> str:
    escaped = value.replace("\\", "\\\\").replace('"', '\\"')
    if (
        not value
        or value.upper() == "NULL"
        or any(character in value for character in ',{}"\\')
        or value[0].isspace()
        or value[-1].isspace()
    ):
        return f'"{escaped}"'
    return escaped


def _array_scalar(value: Any) -> str:
    if value is None:
        return "NULL"
    if isinstance(value, bool):
        return "t" if value else "f"
    if isinstance(value, (int, float, Decimal)) and not isinstance(value, bool):
        return str(value)
    if isinstance(value, (_datetime.date, _datetime.time, _datetime.datetime, _uuid.UUID)):
        return _array_quote(value.isoformat() if hasattr(value, "isoformat") else str(value))
    if isinstance(value, str):
        return _array_quote(value)
    if isinstance(value, (list, tuple)):
        return "{" + ",".join(_array_scalar(item) for item in value) + "}"
    raise TypeError(f"unsupported PostgreSQL array element type: {type(value).__name__}")


def _inferred_array_oid(values: Sequence[Any]) -> int:
    array_oids = {
        bool: 1000,
        int: 1016,
        float: 1022,
        str: 1009,
        Decimal: 1231,
        _datetime.date: 1182,
        _datetime.time: 1183,
        _datetime.datetime: 1115,
        _uuid.UUID: 2951,
    }
    present = [value for value in values if value is not None]
    if not present:
        return 0
    first_type = type(present[0])
    if any(type(value) is not first_type for value in present):
        return 0
    return array_oids.get(first_type, 0)


def encode_parameter(value: Any) -> EncodedParameter:
    """Encode one Python value for PostgreSQL's extended query protocol."""

    if value is None:
        return EncodedParameter(0, TEXT, True, b"")
    with _decoder_lock:
        custom = _custom_encoders.get(type(value))
    if custom is not None:
        encoded = custom(value)
        if not isinstance(encoded, EncodedParameter):
            raise TypeError("custom encoder must return EncodedParameter")
        return encoded
    if isinstance(value, bool):
        return _text(BOOL, "true" if value else "false")
    if isinstance(value, int):
        if not -(1 << 63) <= value < (1 << 63):
            return _text(NUMERIC, str(value))
        return _text(INT8, str(value))
    if isinstance(value, float):
        if math.isnan(value):
            return _text(FLOAT8, "NaN")
        if math.isinf(value):
            return _text(FLOAT8, "Infinity" if value > 0 else "-Infinity")
        return _text(FLOAT8, repr(value))
    if isinstance(value, Decimal):
        return _text(NUMERIC, str(value))
    if isinstance(value, str):
        return _text(TEXT_OID, value)
    if isinstance(value, (bytes, bytearray, memoryview)):
        return EncodedParameter(BYTEA, BINARY, False, bytes(value))
    if isinstance(value, _datetime.datetime):
        oid = TIMESTAMPTZ if value.tzinfo is not None else TIMESTAMP
        return _text(oid, value.isoformat(sep=" "))
    if isinstance(value, _datetime.date):
        return _text(DATE, value.isoformat())
    if isinstance(value, _datetime.time):
        return _text(TIME, value.isoformat())
    if isinstance(value, _datetime.timedelta):
        return _text(INTERVAL, _encode_interval(value))
    if isinstance(value, _uuid.UUID):
        return _text(UUID, str(value))
    if isinstance(value, Json):
        return _text(
            JSONB,
            _json.dumps(value.value, ensure_ascii=False, separators=(",", ":")),
        )
    if isinstance(value, Mapping):
        return _text(
            JSONB,
            _json.dumps(value, ensure_ascii=False, separators=(",", ":")),
        )
    if isinstance(value, Array):
        return _text(value.oid or _inferred_array_oid(value.values), _array_scalar(value.values))
    if isinstance(value, (list, tuple)):
        return _text(_inferred_array_oid(value), _array_scalar(value))
    raise TypeError(f"unsupported PostGamma parameter type: {type(value).__name__}")


def encode_parameters(values: Iterable[Any] | None) -> tuple[tuple[int, int, bool, bytes], ...]:
    if values is None:
        return ()
    if isinstance(values, (str, bytes, bytearray, memoryview, Mapping)):
        raise TypeError("query parameters must be a positional sequence")
    return tuple(encode_parameter(value).native() for value in values)


def _decode_text(data: bytes, _format: int) -> str:
    return data.decode("utf-8")


def _decode_bool(data: bytes, format: int) -> bool:
    if format == BINARY:
        if data in {b"\x00", b"\x01"}:
            return data == b"\x01"
        raise ValueError("invalid binary PostgreSQL boolean")
    return data in {b"t", b"true", b"TRUE", b"1"}


def _decode_int(data: bytes, format: int) -> int:
    if format == BINARY:
        return int.from_bytes(data, "big", signed=True)
    return int(data)


def _decode_float(data: bytes, format: int) -> float:
    if format == BINARY:
        raise ValueError("binary floating-point decoding is not enabled")
    return float(data)


def _decode_decimal(data: bytes, format: int) -> Decimal:
    if format == BINARY:
        raise ValueError("binary numeric decoding is not enabled")
    return Decimal(data.decode("ascii"))


def _decode_bytea(data: bytes, format: int) -> bytes:
    if format == BINARY:
        return data
    if data.startswith(b"\\x"):
        return bytes.fromhex(data[2:].decode("ascii"))
    return data


def _decode_date(data: bytes, format: int) -> _datetime.date:
    if format == BINARY:
        raise ValueError("binary date decoding is not enabled")
    return _datetime.date.fromisoformat(data.decode("ascii"))


def _decode_time(data: bytes, format: int) -> _datetime.time:
    if format == BINARY:
        raise ValueError("binary time decoding is not enabled")
    return _datetime.time.fromisoformat(data.decode("ascii"))


def _decode_datetime(data: bytes, format: int) -> _datetime.datetime:
    if format == BINARY:
        raise ValueError("binary timestamp decoding is not enabled")
    text = data.decode("ascii").replace(" ", "T", 1)
    return _datetime.datetime.fromisoformat(text)


_INTERVAL = re.compile(
    r"^(?P<sign>-)?(?P<days>\d+) days? "
    r"(?P<hours>\d{2}):(?P<minutes>\d{2}):"
    r"(?P<seconds>\d{2})(?:\.(?P<fraction>\d{1,6}))?$"
)


def _decode_interval(data: bytes, format: int) -> _datetime.timedelta:
    if format == BINARY:
        raise ValueError("binary interval decoding is not enabled")
    match = _INTERVAL.fullmatch(data.decode("ascii"))
    if match is None:
        raise ValueError("interval contains months, years, or a non-ISO style")
    values = match.groupdict()
    result = _datetime.timedelta(
        days=int(values["days"]),
        hours=int(values["hours"]),
        minutes=int(values["minutes"]),
        seconds=int(values["seconds"]),
        microseconds=int((values["fraction"] or "0").ljust(6, "0")),
    )
    return -result if values["sign"] else result


def _decode_uuid(data: bytes, format: int) -> _uuid.UUID:
    if format == BINARY:
        return _uuid.UUID(bytes=data)
    return _uuid.UUID(data.decode("ascii"))


def _decode_json(data: bytes, format: int) -> Any:
    if format == BINARY and data.startswith(b"\x01"):
        data = data[1:]
    return _json.loads(data.decode("utf-8"))


_ARRAY_ELEMENTS = {
    1000: BOOL,
    1001: BYTEA,
    1005: INT2,
    1007: INT4,
    1009: TEXT_OID,
    1015: VARCHAR,
    1016: INT8,
    1021: FLOAT4,
    1022: FLOAT8,
    1028: OID,
    1115: TIMESTAMP,
    1182: DATE,
    1183: TIME,
    1185: TIMESTAMPTZ,
    1187: INTERVAL,
    1231: NUMERIC,
    199: JSON,
    2951: UUID,
    3807: JSONB,
}


def _parse_array(text: str) -> list[Any]:
    position = 0

    def parse_level() -> list[Any]:
        nonlocal position
        if position >= len(text) or text[position] != "{":
            raise ValueError("invalid PostgreSQL array")
        position += 1
        result: list[Any] = []
        if position < len(text) and text[position] == "}":
            position += 1
            return result
        while position < len(text):
            if text[position] == "{":
                value: Any = parse_level()
            elif text[position] == '"':
                position += 1
                characters: list[str] = []
                while position < len(text):
                    character = text[position]
                    position += 1
                    if character == '"':
                        break
                    if character == "\\":
                        if position >= len(text):
                            raise ValueError("invalid PostgreSQL array escape")
                        character = text[position]
                        position += 1
                    characters.append(character)
                else:
                    raise ValueError("unterminated PostgreSQL array string")
                value = "".join(characters)
            else:
                start = position
                while position < len(text) and text[position] not in ",}":
                    position += 1
                token = text[start:position]
                value = None if token == "NULL" else token
            result.append(value)
            if position >= len(text):
                raise ValueError("unterminated PostgreSQL array")
            delimiter = text[position]
            position += 1
            if delimiter == "}":
                return result
            if delimiter != ",":
                raise ValueError("invalid PostgreSQL array delimiter")
        raise ValueError("unterminated PostgreSQL array")

    parsed = parse_level()
    if position != len(text):
        raise ValueError("trailing data in PostgreSQL array")
    return parsed


def _decode_array(data: bytes, format: int, element_oid: int) -> list[Any]:
    if format == BINARY:
        raise ValueError("binary PostgreSQL array decoding is not enabled")
    parsed = _parse_array(data.decode("utf-8"))

    def convert(value: Any) -> Any:
        if value is None:
            return None
        if isinstance(value, list):
            return [convert(item) for item in value]
        return decode_value(element_oid, TEXT, value.encode("utf-8"))

    return convert(parsed)


_DECODERS: dict[int, Decoder] = {
    BOOL: _decode_bool,
    BYTEA: _decode_bytea,
    INT2: _decode_int,
    INT4: _decode_int,
    INT8: _decode_int,
    OID: _decode_int,
    FLOAT4: _decode_float,
    FLOAT8: _decode_float,
    NUMERIC: _decode_decimal,
    TEXT_OID: _decode_text,
    VARCHAR: _decode_text,
    18: _decode_text,
    19: _decode_text,
    1042: _decode_text,
    DATE: _decode_date,
    TIME: _decode_time,
    TIMESTAMP: _decode_datetime,
    TIMESTAMPTZ: _decode_datetime,
    INTERVAL: _decode_interval,
    UUID: _decode_uuid,
    JSON: _decode_json,
    JSONB: _decode_json,
}


def decode_value(oid: int, format: int, data: bytes) -> Any:
    """Decode one losslessly copied value from the C boundary."""

    with _decoder_lock:
        decoder = _custom_decoders.get(oid) or _DECODERS.get(oid)
    if decoder is not None:
        try:
            return decoder(data, format)
        except (UnicodeError, ValueError, OverflowError):
            return RawValue(oid, format, data)
    element_oid = _ARRAY_ELEMENTS.get(oid)
    if element_oid is not None:
        try:
            return _decode_array(data, format, element_oid)
        except (UnicodeError, ValueError, OverflowError):
            return RawValue(oid, format, data)
    return RawValue(oid, format, data)
