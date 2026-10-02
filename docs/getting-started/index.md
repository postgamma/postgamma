# Python quickstart

In this tutorial you will create a persistent PostgreSQL cluster, insert a JSON
value, close the database, and reopen it. No PostgreSQL server, `initdb`
command, host name, port, or password is required.

!!! tip "Can I connect before the database exists?"

    Yes. `postgamma.connect("agent.pgm")` opens an existing cluster or creates
    a missing one by default. A string is sufficient; `pathlib.Path` is also
    accepted. You do not create the cluster directory yourself.

## 1. Install PostGamma

Install the wheel that matches your CPython version and Linux platform:

```bash
python -m pip install postgamma
```

To install a wheel downloaded from a release page instead, pass its actual
filename to `pip`, for example
`python -m pip install ./postgamma-{{ POSTGAMMA_VERSION }}-*.whl`.

Verify that Python can load the embedded kernel:

```bash
python -c "import postgamma; print(postgamma.library_info())"
```

This command reads the library bundled in the wheel. It does not contact an
external PostgreSQL installation.

## 2. Create and query a cluster

Save the following program as `quickstart.py`:

```python
--8<-- "examples/python/quickstart.py"
```

Run it:

```bash
python quickstart.py
```

The application output is:

```text
(1, {'kind': 'plan', 'score': 0.98})
PostgreSQL cluster directory: agent.pgm
```

Development builds may also route PostgreSQL log records to standard error.
Those messages come from threads in this process, not from a separate server.

## 3. Understand the first `connect()` call

The first `postgamma.connect(cluster, autocommit=True)` block in the example
performs the complete setup before it creates the table:

| Input state | Result |
| --- | --- |
| `agent.pgm` is missing | Initialize a PostgreSQL 19 cluster there, then connect |
| It is an existing compatible cluster | Open it without replacing data |
| It is a nonempty unrelated directory | Raise `OperationalError` without deleting anything |
| Its parent directory is missing | Raise `OperationalError` |

The new cluster initially contains:

- the PostgreSQL logical database `postgres`;
- the PostgreSQL role `postgamma`;
- system catalogs, WAL, configuration, and table storage.

The first argument is the **cluster directory**. It is not the name of the SQL
database inside that cluster. Read [Database paths and creation](database-paths.md)
before selecting a production location.

## 4. Close and reopen safely

Leaving the `with` block closes the logical connection. If it is the last
top-level connection for this path, PostGamma also shuts down the process-local
embedded instance. Committed files remain in `agent.pgm`.

The second call explicitly requires an existing cluster:

```python
with postgamma.connect(
    cluster,
    mode=postgamma.OpenMode.OPEN_EXISTING,
    autocommit=True,
) as connection:
    row = connection.execute(
        "select id, body from memory where id = $1",
        [1],
    ).fetchone()
```

`OPEN_EXISTING` means “open an existing cluster and fail if it is missing.” Use
it in production when a misspelled path must not create an empty cluster.

## 5. Choose transaction behavior

The example uses `autocommit=True`, so each statement commits independently.
The default is `autocommit=False`; the connection context then commits a clean
body or rolls it back when the body raises, and finally closes the connection:

```python
with postgamma.connect(
    "agent.pgm", mode=postgamma.OpenMode.OPEN_EXISTING
) as connection:
    connection.execute(
        "update memory set body = $1 where id = $2",
        [{"kind": "done"}, 1],
    )
# The update is committed, then the connection closes.
```

Use `transaction()` when you want several units of work on one long-lived
connection:

```python
connection = postgamma.connect(
    "agent.pgm", mode=postgamma.OpenMode.OPEN_EXISTING
)
try:
    with connection.transaction() as transaction:
        transaction.execute(
            "update memory set body = $1 where id = $2",
            [{"kind": "verified"}, 1],
        )
finally:
    connection.close()
```

For explicit lifecycle and management control, keep one instance open:

```python
with postgamma.Database(
    "agent.pgm", mode=postgamma.OpenMode.OPEN_EXISTING
) as database:
    with database.connect() as first, database.connect() as second:
        print(first.execute("select 1").fetchone())
        print(second.execute("select 2").fetchone())
```

See [Connections and concurrency](../concepts/concurrency.md) before running
concurrent transactions.

## Next steps

- [Install a Python wheel or C SDK](installation.md)
- [Understand cluster paths and logical databases](database-paths.md)
- [Use parameters, results, and DB-API cursors](../python/database-and-dbapi.md)
- [Handle transactions](../concepts/transactions.md)
- [Build a vector search with bundled pgvector](../extensions/pgvector.md)
- [Troubleshoot startup and connection errors](../troubleshooting.md)
