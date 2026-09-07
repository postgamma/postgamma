# Troubleshooting

Start with the failing boundary: package loading, cluster opening, SQL
execution, concurrency, or native linking. PostGamma errors preserve both a
host status and PostgreSQL diagnostics so applications do not need to parse one
message string.

## A source build fails

Run the preflight for the requested artifact before inspecting downstream
compiler errors:

```bash
make doctor             # complete local development environment
make doctor-threaded    # threaded PostgreSQL generation and compilation
make doctor-sdk         # native kernel and static C SDK
make doctor-python      # local wheel
make doctor-docs        # documentation
```

The preflight verifies the supported Linux, x86-64, and glibc boundary; pinned
submodules; matching LLVM/Clang LibTooling; C11, C++17, PIC, and pthread
compilation; selected PostgreSQL libraries; ELF tools; Python development
headers; host resources; and tracing permissions. It prints one consolidated
apt or dnf suggestion when packages are missing.

PostGamma requires GNU Make 4.3 or newer because its graph uses grouped target
rules. The older Make version accepted by a standalone PostgreSQL build is not
enough for this superproject.

If the checkout is incomplete, initialize it rather than compiling an empty
submodule directory:

```bash
git submodule update --init --recursive
```

Compiler flags and library paths inherited from Conda, SDK shells, or another
project can combine otherwise valid but incompatible headers and libraries.
Review `CC`, `CXX`, `CPPFLAGS`, `CFLAGS`, `CXXFLAGS`, `LDFLAGS`, `LIBS`,
`PKG_CONFIG_PATH`, `LLVM_CONFIG`, and `CLANGXX`. In particular, avoid
`-Werror`, `-march=native`, and `-static` unless the resulting portability and
product-boundary changes are intentional. Re-run the doctor with the same
variable assignments used for the build.

When PostgreSQL configuration fails, inspect the applicable `config.log` below
`build/`. The configuration wrapper fingerprints relevant arguments and tools
and recreates a stale managed build tree when that identity changes. Use
`make clean` when deliberately starting over; do not edit generated PostgreSQL
trees or copy configuration files between hosts.

## Record diagnostic fields

For Python, log the structured fields on every `postgamma.Error`:

```python
import postgamma

try:
    with postgamma.connect(
        "data/agent.pgm", mode=postgamma.OpenMode.OPEN_EXISTING
    ) as connection:
        connection.execute("select * from missing_table")
except postgamma.Error as error:
    print(
        {
            "type": type(error).__name__,
            "status": error.status_name,
            "sqlstate": error.sqlstate,
            "message": str(error),
            "detail": error.detail,
            "hint": error.hint,
        }
    )
```

For C, inspect the returned `pgm_status` and, when present,
`pgm_error_message()`, `pgm_error_sqlstate()`, `pgm_error_detail()`, and
`pgm_error_field()`. Free the error with `pgm_error_free()`.

## Python cannot import PostGamma

### `ModuleNotFoundError: No module named 'postgamma'`

Confirm that the wheel was installed into the interpreter running the
application:

```bash
python -m pip show postgamma
python -c "import sys; print(sys.executable)"
```

Do not mix a virtual environment's `pip` with another interpreter.

### Native extension load failure

Check the wheel platform tag and the host:

```bash
python -c "import platform; print(platform.machine(), platform.libc_ver())"
python -m pip debug --verbose
```

The first release supports Linux x86-64 and glibc 2.28 or newer. A wheel for a
different CPython ABI cannot be loaded.

### Runtime resources are unavailable

An official wheel includes its resource pack. This error normally means the
wheel was unpacked or copied incompletely. Reinstall it rather than copying the
Python package directory by hand.

Source-tree developers may set `POSTGAMMA_RESOURCE_ROOT` to a complete matching
resource pack. Production wheel users should not need this override.

## A cluster does not open

### The cluster path is missing

`OPEN_OR_CREATE` is the default. It creates the final cluster directory only
when its parent exists:

```python
import os

os.makedirs("data", exist_ok=True)
connection = postgamma.connect("data/agent.pgm")
```

With `OPEN_EXISTING`, a missing final directory is an error by design.

### The destination is nonempty but is not a cluster

PostGamma never empties or replaces an unrelated nonempty directory. Choose a
new path or inspect the existing directory manually. Do not implement recovery
by recursively deleting an unknown path.

### The cluster is busy

Only one live embedded instance may own a cluster directory. Repeated top-level
`connect()` and `connect_async()` calls in one process automatically share that
instance and return independent sessions. If their instance settings differ,
PostGamma raises `InterfaceError`; pass the same settings or use one explicitly
managed instance:

```python
with postgamma.Database(
    "data/agent.pgm", mode=postgamma.OpenMode.OPEN_EXISTING
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

Another operating-system process cannot open the same cluster concurrently.
Close the owner instead of deleting `.postgamma.lock` while it is live.

### PostgreSQL major version mismatch

PostGamma 19 opens PostgreSQL 19 clusters only. Use the matching build or make a
logical dump with the old version and restore it into a newly initialized
cluster. Do not copy a physical cluster between PostgreSQL majors.

### The logical database does not exist

The cluster path may be valid while `database` names an absent PostgreSQL
database. Connect to the built-in `postgres` database with autocommit enabled,
then create the database explicitly:

```python
with postgamma.connect(
    "data/agent.pgm",
    mode=postgamma.OpenMode.OPEN_EXISTING,
    autocommit=True,
) as connection:
    connection.execute("create database application")
```

Then reconnect with `database="application"`. `connect()` never creates
an arbitrary logical database implicitly.

## A SQL operation fails

### Parameter marker error

Native connection methods use PostgreSQL markers:

```python
connection.execute("select $1::int + $2::int", [20, 22])
```

DB-API cursors advertise `paramstyle = "numeric"` and use `:1`, `:2`, and later
markers:

```python
cursor.execute("select :1::int + :2::int", [20, 22])
```

Do not interpolate values with Python string formatting.

### Transaction is aborted after an error

PostgreSQL keeps a failed transaction in the error state until rollback:

```python
try:
    connection.execute("insert into items values ($1)", [duplicate_id])
except postgamma.IntegrityError:
    connection.rollback()
```

Do not issue unrelated statements on that connection before rolling back.

### Materialized result is too large

Materialized results default to a 16 MiB limit and individual values to 8 MiB.
Use `Connection.stream()` for large row sets or COPY for bulk transfer instead
of raising the limit without a memory budget.

### Timeout or cancellation

`QueryTimeoutError` means the host deadline expired. `QueryCanceledError` means
the request was canceled. Wait for request retirement before reusing or closing
the connection. If cancellation cannot restore the protocol safely, close that
connection and open another one.

## Concurrent work stops making progress

Open transactions retain executor workers. If every worker is pinned, another
session waits until a transaction commits or rolls back.

The hard capacity rule is:

```text
simultaneously open transactions <= worker_count
```

Inspect `connection.status.pinned`, `pin_reasons`, and instance telemetry. Keep
transactions short or increase `worker_count` before opening the instance.

Calls through one connection are serialized. Use separate connections for
concurrent work.

## A forked child rejects a handle

Database handles inherited through `fork()` are intentionally invalid. Open
PostGamma after forking, or close the parent instance first:

```python
database.close_before_fork()
```

The child must construct a new `Database`. PostGamma does not attempt to repair
inherited PostgreSQL threads and locks.

## A static C application does not link

### Undefined zlib, ICU, math, or pthread symbols

Use the complete static `pkg-config` flags:

```bash
cc -std=c11 app.c \
  $(pkg-config --cflags --static --libs postgamma) \
  -o app
```

### `postgamma.pc` is not found

```bash
export PKG_CONFIG_PATH="/opt/postgamma-sdk/lib/pkgconfig"
pkg-config --modversion postgamma
```

### The executable unexpectedly needs `libpostgamma.so`

Inspect the actual link command and ELF dependencies:

```bash
readelf -d ./app
ldd ./app
```

Use the static SDK prefix and `pkg-config --static`. A supported static consumer
has no `NEEDED` entry for `libpostgamma.so`.

### It links on one Linux host but not another

The archive may reference a newer glibc than the target provides. Rebuild
against the oldest supported sysroot. Copying a `.a` file does not erase the
libc functions chosen when its objects were compiled.

## Report a reproducible problem

Open a public issue through the repository's issue tracker after searching for
an existing report. Suspected vulnerabilities must use the private channel in
the repository's `SECURITY.md` policy, not a public issue.

Include:

- PostGamma package or C ABI version;
- `library_info()` or the C identity functions;
- PostgreSQL major;
- operating system, architecture, libc, Python, and compiler versions;
- the exception type, status name, SQLSTATE, detail, and hint;
- whether the cluster was new, reopened, or recovered after a crash;
- the smallest program that reproduces the problem;
- whether multiple threads, multiple connections, asyncio, or `fork()` were
  involved.

Never attach a production cluster directory unless its data is intentionally
safe to disclose.
