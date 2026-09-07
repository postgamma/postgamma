# PostGamma

[postgamma.com](https://postgamma.com)

PostGamma embeds PostgreSQL 19 directly inside a CPython process. It starts no
database server process, opens no network socket, and exposes ordinary Python
`Database` and `Connection` objects backed by a thread-safe in-process kernel.
The wheel contains one CPython extension with the PostGamma kernel linked into
it statically; it does not ship or load a separate `libpostgamma.so`.

The source distribution contains the complete documentation under `docs/`.

Install a downloaded release wheel:

```console
python -m pip install postgamma
```

```python
import postgamma

cluster = "agent.pgm"

with postgamma.connect(cluster, autocommit=True) as connection:
    connection.execute(
        "create table if not exists memory("
        "id bigint primary key, body jsonb not null)"
    )
    connection.execute(
        "insert into memory values ($1, $2) "
        "on conflict (id) do update set body = excluded.body",
        [1, {"kind": "plan", "score": 0.98}],
    )
    print(connection.execute("select * from memory").fetchall())
```

The first argument may be a string or any `os.PathLike` object. It names a
PostgreSQL cluster directory, not a DSN or SQL database name. The default
`OPEN_OR_CREATE` mode initializes a missing PostgreSQL 19 cluster and opens an
existing compatible one without replacing data. PostGamma creates the final
cluster directory; only its parent must already exist. The initial SQL database
is `postgres` and the initial role is `postgamma`.

Use `OPEN_EXISTING` when an absent or misspelled production path must fail:

```python
with postgamma.connect(
    "agent.pgm", mode=postgamma.OpenMode.OPEN_EXISTING
) as connection:
    print(connection.execute("select current_database()").fetchone())
```

Repeated top-level calls for the same canonical path share one process-local
embedded instance and return independent sessions. The last connection closes
that implicit instance. Use `Database` when you want explicit ownership,
management operations, or an instance event stream.

The package includes DB-API 2.0 compatibility, synchronous and caller-driven
`asyncio` APIs, bounded row and COPY streaming, prepared statements, routed
events, logical dump and restore, management operations, and Arrow C Data
export. Separate connections can execute concurrently without holding the GIL.

Use DB-API 2.0 when an existing framework expects it:

```python
import postgamma

connection = postgamma.connect("agent.pgm")
try:
    cursor = connection.cursor()
    cursor.execute("select :1::int + :2::int", [20, 22])
    print(cursor.fetchone())
finally:
    connection.close()
```

Use the native asyncio facade for event-loop applications. Query progress is
driven by native waitables and does not dedicate one Python helper thread per
request.

This alpha targets Linux x86-64 with glibc 2.28 or newer and CPython 3.10
through 3.14. It embeds PostgreSQL 19 only. A materialized result is limited to
16 MiB by default and one value to 8 MiB; use row streaming or COPY for larger
workloads. Inherited database handles fail closed after `fork()`: open lazily
after the fork or call `Database.close_before_fork()` first.

This release provides the embedded PostgreSQL kernel, its public Python
adapters, and bundled pgvector 0.8.6. Enable vector types and indexes once per
logical database with `CREATE EXTENSION vector`; no separate native library is
loaded.
