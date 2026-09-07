# Python type mapping

PostGamma binds parameters through PostgreSQL's extended query protocol and
decodes result values by PostgreSQL type OID. It never guesses an unknown OID.

## Built-in mappings

| PostgreSQL type | Python input | Python result | Notes |
| --- | --- | --- | --- |
| SQL `NULL` | `None` | `None` | The target type is inferred by PostgreSQL or an explicit cast |
| `bool` | `bool` | `bool` | Text and binary results are supported |
| `int2`, `int4`, `int8`, `oid` | `int` | `int` | Input within signed 64-bit range is sent as `int8`; larger input is sent as `numeric` |
| `float4`, `float8` | `float` | `float` | `NaN` and infinities use PostgreSQL spellings |
| `numeric` | `Decimal`, large `int` | `Decimal` | Decimal precision is preserved in text format, including `NaN` |
| `text`, `varchar`, `bpchar`, `name`, `char` | `str` | `str` | UTF-8 at the Python boundary |
| `bytea` | `bytes`, `bytearray`, `memoryview` | `bytes` | Parameters use binary format |
| `date` | `datetime.date` | `datetime.date` | ISO text representation |
| `time` | `datetime.time` | `datetime.time` | `time without time zone`; unsupported time forms remain raw |
| `timestamp` | naive `datetime.datetime` | naive `datetime.datetime` | No time-zone conversion is added |
| `timestamptz` | aware `datetime.datetime` | aware `datetime.datetime` | PostgreSQL renders in the session `TimeZone`; the returned offset is preserved |
| `interval` | `datetime.timedelta` | `datetime.timedelta` | Day/time intervals are supported; months, years, or unsupported styles remain raw |
| `uuid` | `uuid.UUID` | `uuid.UUID` | Text and binary results are supported |
| `json`, `jsonb` | mapping or `Json(value)` | Python JSON values | A mapping is encoded as `jsonb`; wrap scalar or list JSON explicitly |
| Supported arrays | sequence or `Array(values, oid=...)` | nested `list` | NULL elements and dimensions are preserved when the text form is supported |

Python `int` does not overflow when reading `int8`. When writing, values outside
the signed 64-bit range are encoded as PostgreSQL `numeric`; an explicit SQL
cast can still reject a value that does not fit the target column.

## JSON versus arrays

A Python `list` or `tuple` means a PostgreSQL array by default. Use `Json()`
when the intended parameter is a JSON array or scalar:

```python
connection.execute(
    "insert into events(payload, labels) values ($1, $2)",
    [
        postgamma.Json([{"kind": "created"}]),
        postgamma.Array(["agent", "local"]),
    ],
)
```

`Json()` encodes as `jsonb`. Both `json` and `jsonb` result columns decode with
Python's JSON decoder.

## Timestamps

PostgreSQL `timestamp without time zone` does not identify an instant and maps
to a naive Python `datetime`. `timestamptz` maps to an aware `datetime` using
the offset rendered under the connection's current `TimeZone` setting.

Set the session time zone explicitly when stable presentation matters:

```python
connection = database.connect(settings={"timezone": "UTC"})
```

## Arrays

The built-in decoder covers arrays of booleans, byte strings, integers, floats,
text, dates, times, timestamps, intervals, numerics, JSON values, and UUIDs.
Nested arrays become nested lists and SQL NULL elements become `None`.

An empty or all-NULL input sequence has no inferable element OID. Cast the
parameter in SQL or pass `Array(values, oid=...)`. PostgreSQL validates element
types and rectangular dimensions.

## Unknown and unsupported values

An unknown OID or a known type whose representation cannot be decoded is
returned losslessly as:

```python
postgamma.RawValue(oid=42424, format=0, data=b"...")
```

Use `RawValue.text()` only when `format == 0`. Register a process-wide custom
codec for an application or bundled-extension type; see
[Events, codecs, and Arrow](integration.md#custom-codecs).
