# Database paths and creation

PostGamma is used like an embedded database, but PostgreSQL persistence is a
cluster directory rather than one file. This page defines exactly what happens
for every path state and explains the separate SQL database name.

## One path represents one PostgreSQL cluster

```python
cluster = "agent.pgm"
```

A plain string is the normal input. `pathlib.Path` and other `os.PathLike`
objects are accepted too, but they are not required.

The `.pgm` suffix is only an application convention. After initialization,
`cluster` is a directory containing entries such as:

```text
agent.pgm/
├── PG_VERSION
├── base/
├── global/
├── pg_wal/
└── postgresql.conf
```

Treat the entire directory as one persistent unit. Do not copy, rename, or
partially replace it while an instance is open. For portable backup and
major-version migration, use PostGamma's logical dump and restore API.

Use an absolute path in services and agents whose working directory may change:

```python
cluster = "/var/lib/my-agent/postgamma"
```

A relative path is resolved against the process working directory when the
embedded instance first opens, not when the `Database` object is constructed.

## Explicit open modes

Both `postgamma.connect()` and `Database()` accept an explicit `mode`. The
default is `OpenMode.OPEN_OR_CREATE`.

| Path state | `OPEN_OR_CREATE` | `OPEN_EXISTING` | `CREATE_NEW` |
| --- | --- | --- | --- |
| Missing final directory, writable parent | Initialize and open | Fail | Initialize and open |
| Existing empty directory | Initialize and atomically replace | Fail | Fail |
| Compatible PostgreSQL 19 cluster | Open unchanged | Open unchanged | Fail |
| Nonempty unrelated or incompatible directory | Fail without overwriting | Fail | Fail |

```python
postgamma.connect("agent.pgm")
postgamma.connect("agent.pgm", mode=postgamma.OpenMode.OPEN_EXISTING)
postgamma.connect("new-agent.pgm", mode=postgamma.OpenMode.CREATE_NEW)
```

`OPEN_OR_CREATE` never means truncate, reset, or recreate. `CREATE_NEW` is the
strict creation form: any pre-existing final path is an error. Its absent-path
check runs under the same creation lock as atomic publication, so concurrent
creators cannot both succeed.

Repeated top-level `connect()` and `connect_async()` calls for the same
canonical path reuse one process-local instance and return independent logical
sessions. Their instance settings and worker count must agree. A separately
constructed `Database` cannot independently own that path while the implicit
instance is live, and another operating-system process cannot open it either.

Do not create the final cluster directory yourself. For a database in the
current directory, this is the complete setup:

```python
with postgamma.connect("agent.pgm") as connection:
    print(connection.execute("select current_database()").fetchone())
```

As with file-backed SQLite and DuckDB databases, a missing parent hierarchy is
not created implicitly. Create only the parent when selecting a nested path:

```python
import os

os.makedirs("data", exist_ok=True)

with postgamma.connect("data/agent.pgm") as connection:
    print(connection.execute("select current_database()").fetchone())
```

Cluster creation uses a private staging directory and atomic rename. A failure
before publication does not expose a half-initialized target. A failure after
publication reports that the new cluster remains available for a later retry.

## Cluster path versus SQL database name

This call has two different database-related values:

```python
connection = postgamma.connect(
    "data/agent.pgm",      # PostgreSQL cluster directory
    database="postgres",  # SQL database inside that cluster
)
```

The initial cluster contains:

- role `postgamma`, used by default;
- SQL database `postgres`, used by default;
- PostgreSQL template databases and system catalogs.

PostGamma automatically creates the cluster directory, but it does not
implicitly create an arbitrary value passed as `database`. PostgreSQL
returns an error if that logical database does not exist.

## Create another SQL database

Most embedded applications can use the default `postgres` database and create
their own tables or schemas. If isolation requires another PostgreSQL database,
create it explicitly from `postgres` first. `CREATE DATABASE` must run in
autocommit mode:

```python
cluster = "data/agent.pgm"

with postgamma.connect(cluster, autocommit=True) as administrator:
    found = administrator.execute(
        "select 1 from pg_database where datname = $1",
        ["application"],
    ).fetchone()
    if found is None:
        administrator.execute("create database application")

with postgamma.connect(
    cluster,
    mode=postgamma.OpenMode.OPEN_EXISTING,
    database="application",
) as connection:
    connection.execute("create table jobs(id bigint primary key, state text)")
```

The second context commits the table creation on a clean exit. Dynamic database
names are SQL identifiers and cannot be supplied through `$1`; quote or validate
application-controlled identifiers before constructing such DDL.

## Users and authentication

The default user is `postgamma`. The Python API accepts another existing role
through `user="application_role"`, but it does not create that role
automatically.

There is no host, port, or password argument because the public Python path does
not open a network endpoint or consult `pg_hba.conf`. Access to the embedded
kernel is controlled by the host process and filesystem ownership. PostgreSQL
roles still define SQL privileges within the cluster.

## Open, close, and reopen

`Database()` construction is lazy:

```python
database = postgamma.Database("data/agent.pgm")
assert not database.opened
database.open()
assert database.opened
database.close()
```

Entering a `Database` context, opening a connection, or executing through the
database also triggers the first open. A closed `Database` object is not
reusable; construct a new object to reopen the same persistent directory:

```python
with postgamma.Database(
    "data/agent.pgm", mode=postgamma.OpenMode.OPEN_EXISTING
) as reopened:
    print(reopened.execute("select current_database()").fetchone())
```

Committed data survives close and process exit. Uncommitted work is rolled back
when its connection closes. After a host crash, the next open runs PostgreSQL
WAL recovery before accepting connections.

## Common failures

| Symptom | Likely cause | Action |
| --- | --- | --- |
| Missing-path error | `OPEN_EXISTING`, or a missing parent directory | Create the parent or choose `OPEN_OR_CREATE` intentionally |
| Already-exists error | `CREATE_NEW` and the final path exists | Choose a new path or use another mode intentionally |
| Nonempty destination error | Path contains files but is not a valid cluster | Select a new empty path; PostGamma will not erase it |
| Busy/open error | Another live PostGamma instance owns the directory | Close the owner or use a different cluster |
| Version mismatch/startup error | Cluster is not compatible with PostgreSQL 19 | Use the matching build or logical migration |
| SQL database does not exist | `database` names an absent logical database | Connect to `postgres` and run `CREATE DATABASE` |

Inspect `error.status_name`, `error.sqlstate`, `error.detail`, and `error.hint`
when handling a PostGamma exception. Do not recover from a path error by
deleting an unknown directory automatically.
