# Database and DB-API

This guide documents the normal synchronous path from a cluster directory to
SQL results. For path creation rules, first read
[Database paths and creation](../getting-started/database-paths.md).

## `postgamma.connect()` in plain language

The commonly used arguments are:

```python
postgamma.connect(
    path,
    *,
    mode=postgamma.OpenMode.OPEN_OR_CREATE,
    database="postgres",
    user="postgamma",
    settings=None,
    connection_settings=None,
    worker_count=4,
    autocommit=False,
    timeout=30.0,
)
```

| Argument | Meaning |
| --- | --- |
| `path` | Directory containing one persistent PostgreSQL cluster |
| `mode` | Explicit open policy; open-or-create is the default |
| `database` | Existing SQL database inside the cluster |
| `user` | Existing PostgreSQL role for this logical session |
| `settings` | Instance/startup GUC values |
| `connection_settings` | GUC values for this session only |
| `worker_count` | Maximum carrier workers for the shared path instance |
| `autocommit` | Commit each statement independently when true |
| `timeout` | Default operation deadline in seconds; `None` disables it |

The returned object is already connected. Repeated calls for the same canonical
path share one process-local embedded instance, but each call returns a distinct
PostgreSQL session. Calling `connection.close()` releases that session; the last
top-level connection closes the implicit instance.

There is no host, port, or password option. All PostgreSQL protocol traffic is
carried through memory inside the current process.

## Native connection calls

`Connection.execute()` uses PostgreSQL extended-protocol markers:

```python
with postgamma.connect("data/agent.pgm") as connection:
    result = connection.execute(
        "insert into tasks(name, priority) values ($1, $2) returning id",
        ["index", 10],
    )
    task_id = result.fetchone()[0]
    connection.commit()
```

The explicit `commit()` is needed because `autocommit=False` is the default.
The context manager would also commit on a clean exit, but an explicit commit
can make a long-running scope easier to reason about.

`Result` owns a materialized result and exposes:

| Member | Meaning |
| --- | --- |
| `columns` / `description` | Native and PEP 249 column metadata |
| `rowcount` | Rows in this result or affected by this command |
| `command_status` | PostgreSQL command tag such as `INSERT 0 1` |
| `fetchone()` | Next row or `None` |
| `fetchmany(size)` | Up to `size` remaining rows |
| `fetchall()` | All remaining rows |
| iteration | Remaining rows one at a time from the materialized result |

Materialized results are bounded by default. Use
[row streaming](streaming-and-copy.md#row-streaming) when the result may exceed
the configured limit.

## DB-API 2.0 cursors

The package declares:

```python
postgamma.apilevel == "2.0"
postgamma.threadsafety == 2
postgamma.paramstyle == "numeric"
```

DB-API cursors use `:1`, `:2`, and later markers instead of `$1`:

```python
import postgamma

with postgamma.connect("data/agent.pgm") as connection:
    with connection.cursor() as cursor:
        cursor.execute(
            "select body from memory where id = :1",
            [1],
        )
        print(cursor.fetchone())
```

The marker rewriter understands PostgreSQL quoted strings, quoted identifiers,
dollar quotes, casts, line comments, and nested block comments. Native `$1`
parameters remain the preferred style when calling `Connection.execute()`
directly.

`executemany()` executes once per supplied parameter sequence. `callproc()` is
not currently supported. `setinputsizes()` and `setoutputsize()` are accepted as
no-op DB-API hints.

## Transaction behavior

With the default `autocommit=False`, the first ordinary statement starts a
transaction:

```python
with postgamma.connect("data/agent.pgm") as connection:
    connection.execute("update accounts set balance = balance - $1 where id = $2", [10, 1])
    connection.execute("update accounts set balance = balance + $1 where id = $2", [10, 2])
    connection.commit()
```

The connection context manager applies this policy:

| Exit | Action |
| --- | --- |
| Body returns normally | Commit active transaction, then close |
| Body raises | Roll back active transaction, then close without replacing the original exception |

Use `transaction()` when a connection must stay open across several independent
units of work:

```python
connection = postgamma.connect(
    "data/agent.pgm",
    mode=postgamma.OpenMode.OPEN_EXISTING,
)
try:
    with connection.transaction() as transaction:
        transaction.execute(
            "update accounts set balance = balance - $1 where id = $2",
            [10, 1],
        )
        transaction.execute(
            "update accounts set balance = balance + $1 where id = $2",
            [10, 2],
        )
finally:
    connection.close()
```

A clean transaction scope commits; an exception rolls it back and leaves the
connection open. Scopes must start while the connection is idle. They are
single-use and cannot be nested on one connection.

Use `autocommit=True` for statements that must not run in a transaction block,
including `CREATE DATABASE`, `VACUUM`, and some maintenance commands:

```python
with postgamma.connect(
    "data/agent.pgm",
    mode=postgamma.OpenMode.OPEN_EXISTING,
    autocommit=True,
) as connection:
    connection.execute("vacuum analyze tasks")
```

Do not change `connection.autocommit` while a transaction is active. Commit or
roll back first.

## One instance with several sessions

Top-level `connect()` is sufficient for simultaneous sessions. The path registry
reuses one instance automatically:

```python
first = postgamma.connect("data/agent.pgm", worker_count=4)
second = postgamma.connect("data/agent.pgm", worker_count=4)
assert first.database is second.database
try:
    print(first.execute("select 1").fetchone())
    print(second.execute("select 2").fetchone())
finally:
    first.close()
    second.close()
```

Use an explicit `Database` when lifecycle control, management operations, event
routing, or several connection waves should outlive individual sessions:

```python
with postgamma.Database(
    "data/agent.pgm",
    mode=postgamma.OpenMode.OPEN_EXISTING,
    worker_count=4,
) as database:
    first = database.connect(autocommit=True)
    second = database.connect(autocommit=True)
    try:
        print(first.execute("select 1").fetchone())
        print(second.execute("select 2").fetchone())
    finally:
        first.close()
        second.close()
```

Separate connections may execute concurrently. Calls on one connection are
serialized. Open transactions retain carrier workers, so the number of
simultaneously open transactions must not exceed `worker_count`.

## Convenience execution

`Database.execute()` opens a temporary autocommit connection, executes one
statement, and closes that connection while leaving the database instance open:

```python
with postgamma.Database(
    "data/agent.pgm", mode=postgamma.OpenMode.OPEN_EXISTING
) as database:
    result = database.execute("select count(*) from tasks")
    print(result.fetchone())
```

This is suitable for independent statements. Use an explicit connection when
session state, temporary tables, prepared statements, or a transaction must
span several calls.

## Multiple results and prepared statements

Use `execute_script()` only for simple-protocol, multi-statement behavior:

```python
results = connection.execute_script(
    "create temporary table t(n int); "
    "insert into t values (1), (2); "
    "select * from t order by n"
)
for result in results:
    print(result.kind, result.command_status, result.rows)
```

Ordinary `execute()` accepts one extended-protocol statement. Prepared
statements remain owned by one logical connection:

```python
with connection.prepare(
    "select body from memory where id = $1",
    parameter_type_oids=[20],
) as statement:
    print(statement.description)
    print(statement.execute([1]).fetchone())
```

Close every statement, stream, request, COPY object, and cursor before closing
its connection. Context managers are the preferred ownership pattern.
