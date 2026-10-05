# Events, codecs, and Arrow

## Instance events

PostGamma routes logs, notices, notifications, and queue-overflow records to an
instance-owned event queue. Callbacks run only on the host thread that dispatches
events; PostgreSQL role threads do not call Python.

```python
def report(event: postgamma.Event) -> None:
    print(event.kind, event.connection_id, event)

database.add_event_handler(report)
database.dispatch_events(maximum=100)
```

Use `Database.waitable` with a native event loop, `next_event()` for polling, or
`AsyncDatabase.events()` in asyncio code. A per-query `on_notice` callback may
be supplied to streaming and prepared-statement execution.

Overflow is observable as `OverflowEvent` and telemetry; the queue never hides
dropped control events silently.

## Custom codecs

Unknown PostgreSQL types arrive as `RawValue(oid, format, data)`. Register a
decoder and optional Python encoder explicitly:

```python
class Vector:
    def __init__(self, text: str) -> None:
        self.text = text


postgamma.register_codec(
    oid=42424,
    decoder=lambda data, _format: Vector(data.decode("utf-8")),
    python_type=Vector,
    encoder=lambda value: postgamma.EncodedParameter(
        42424, 0, False, value.text.encode("utf-8")
    ),
)
```

Registrations are process-wide Python adapter state. Unregister test-specific or
temporary codecs when they are no longer valid.

`Json(value)` and `Array(values)` request explicit JSON or PostgreSQL array
parameter encoding when automatic type selection would be ambiguous.

## Arrow C Data

Arrow is an output adapter, not PostGamma's internal execution format:

```python
arrow = connection.execute_arrow("select id, body from memory order by id")
schema_capsule, array_capsule = arrow.__arrow_c_array__()
```

Capsule ownership transfers once. A second export raises `InterfaceError`.
PostGamma does not link an Arrow runtime and does not claim zero-copy conversion
from PostgreSQL row protocol data. There is no `ArrowArrayStream`; use bounded
row streaming for incremental results.

## Capability discovery

Optional surfaces must be checked through `postgamma.capabilities()` when a host
supports multiple PostGamma builds:

```python
if postgamma.capabilities() & postgamma.Capability.ARROW_C_DATA:
    arrow = connection.execute_arrow("select 1")
```

An unavailable capability raises `NotSupportedError`; it does not silently
change semantics.
