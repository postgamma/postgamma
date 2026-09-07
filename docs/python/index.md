# Python overview

The Python package has one embedded kernel with matching synchronous and
asynchronous entry points. Start with `postgamma.connect()` or
`await postgamma.connect_async()`; both return one ready logical connection.

## Choose the entry point

| Need | Use | Who owns the embedded instance? |
| --- | --- | --- |
| One synchronous session | `postgamma.connect(path)` | Process-local path registry |
| One asynchronous session | `await postgamma.connect_async(path)` | The same path registry |
| Explicit instance, multiple sessions, events, or management | `postgamma.Database(path)` | Your `Database` object |
| Async explicit instance and management | `postgamma.AsyncDatabase(path)` | Your `AsyncDatabase` object |

All three accept a **PostgreSQL cluster directory**, not a host, port, DSN, or
single database file. By default a missing cluster is created automatically.

## Simplest persistent connection

```python
import postgamma

cluster = "application.pgm"

with postgamma.connect(cluster, autocommit=True) as connection:
    connection.execute(
        "create table if not exists counters("
        "name text primary key, value bigint not null)"
    )
    connection.execute(
        "insert into counters values ($1, $2) "
        "on conflict (name) do update set value = counters.value + 1",
        ["runs", 1],
    )
    print(
        connection.execute(
            "select value from counters where name = $1", ["runs"]
        ).fetchone()
    )
```

On the first run, `connect()` initializes `application.pgm` in process. On later
runs, it opens the same PostgreSQL 19 cluster. Simultaneous top-level calls for
the same canonical path share one instance and return independent sessions. The
last of those connections to close shuts down the implicit instance.

Read [Database paths and creation](../getting-started/database-paths.md) before
choosing a production path. In particular, use `OpenMode.OPEN_EXISTING` when a
typo must fail instead of creating a fresh cluster.

## Managed instance with multiple connections

`Database` keeps one instance running while connections come and go:

```python
import postgamma

with postgamma.Database(
    "data/application.pgm",
    mode=postgamma.OpenMode.OPEN_EXISTING,
    worker_count=4,
) as database:
    with database.connect(autocommit=True) as first:
        print(first.execute("select current_database()").fetchone())

    with database.connect(autocommit=True) as second:
        print(second.execute("select current_user").fetchone())
```

`Database()` is lazy when used without a context manager. `open()`, `connect()`,
`execute()`, or entering its context starts the instance. The default mode
opens an existing compatible path or creates a missing one.

One `Database` can own more connections than workers. Idle sessions do not
consume a carrier worker permanently, but open transactions do. See
[Connections and concurrency](../concepts/concurrency.md) for the capacity
rule.

## Cluster and connection options

Instance settings apply while the cluster starts:

```python
database = postgamma.Database(
    "data/application.pgm",
    settings={
        "shared_buffers": "64MB",
        "max_connections": 32,
        "log_min_messages": "warning",
    },
)
```

Connection settings apply only to one logical PostgreSQL session:

```python
connection = database.connect(
    database="postgres",
    user="postgamma",
    settings={"search_path": "agent, public", "timezone": "UTC"},
)
```

Top-level `postgamma.connect()` names its first parameter `path`, so `database`
unambiguously selects the logical PostgreSQL database inside the cluster:

```python
connection = postgamma.connect(
    "data/application.pgm",
    database="postgres",
    connection_settings={"timezone": "UTC"},
)
```

An invalid setting, absent SQL database, or absent role is reported during
instance or connection open; it is never ignored silently.

## Parameters and results

The native `Connection.execute()` path uses PostgreSQL `$1`, `$2`, and later
extended-protocol parameters:

```python
result = connection.execute(
    "select $1::int8 + $2::int8 as total",
    [20, 22],
)
print(result.fetchone())  # (42,)
```

`Result` is materialized and provides `fetchone()`, `fetchmany()`, `fetchall()`,
iteration, column metadata, row count, and command status. Use a row stream for
large results.

Built-in codecs cover `None`, booleans, integers, floats, `Decimal`, strings,
bytes, dates, times, timestamps, timedeltas, UUIDs, JSON objects, and arrays. An
unknown PostgreSQL OID remains a lossless `RawValue` instead of being guessed.

## Errors

PostgreSQL diagnostics map to the standard DB-API exception hierarchy. Timeout,
cancellation, and inherited-after-fork failures add:

- `QueryTimeoutError`;
- `QueryCanceledError`;
- `ForkedProcessError`.

Every PostGamma `Error` may carry `status`, `status_name`, `sqlstate`,
`severity`, `detail`, and `hint`. Preserve those fields in application logs;
the primary message alone may not explain a path or SQL failure.

## Continue by task

- [Database and DB-API](database-and-dbapi.md): calls, cursors, transactions,
  results, and ownership.
- [Asyncio](asyncio.md): event-loop execution and cancellation.
- [Streaming and COPY](streaming-and-copy.md): bounded large transfers.
- [Management and logical backup](management.md): checkpoint, maintenance,
  dump, and restore.
