# Streaming and COPY

Materialized results are limited to 16 MiB by default and individual values to
8 MiB. These limits prevent an accidental query from consuming unbounded host
memory. Use row streaming or COPY for larger transfers.

## Row streaming

```python
with connection.stream(
    "select id, body from memory order by id",
    chunk_rows=512,
) as rows:
    for row in rows:
        process(row)
```

`chunk_rows` bounds each native tuple chunk. `fetchone()` and `fetchmany()` pull
incrementally. Calling `fetchall()` on a stream is intentionally explicit and
can still accumulate all remaining rows in Python memory.

A row stream owns the connection's active request until it reaches the terminal
result or closes. Always use a context manager or call `close()`.

## COPY from a file-like object

```python
import io

source = io.BytesIO(b"1,first\n2,second\n")
result = connection.copy_from(
    "copy items from stdin with (format csv)",
    source,
)
print(result.command_status)
```

## COPY to a file-like object

```python
target = io.BytesIO()
connection.copy_to(
    "copy items to stdout with (format csv)",
    target,
)
payload = target.getvalue()
```

`copy_from()` and `copy_to()` are convenience loops over partial native I/O.
For explicit backpressure control, use `connection.copy()` and drive the
returned `CopyIn` or `CopyOut` object directly.

## Async streaming

```python
stream = await connection.stream("select * from items", chunk_rows=256)
try:
    while row := await stream.fetchone():
        process(row)
finally:
    await stream.aclose()
```

Async COPY exposes `write()`, `read()`, `copy_from()`, and `copy_to()` without
changing the underlying bounded transport contract.

Abandoning a synchronous or asynchronous stream or COPY object emits a
`ResourceWarning` when Python collects it and makes a best-effort attempt to
release the request. Finalizers are a diagnostic safety net, not a lifecycle
strategy: use `with` / `async with`, `close()`, or `aclose()` so cleanup errors
remain visible at the operation site.

!!! note

    COPY BOTH and the PostgreSQL replication protocol are not part of the
    current API.
