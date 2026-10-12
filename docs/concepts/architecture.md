# Architecture

PostGamma treats an unmodified PostgreSQL source tree as compiler input. A
Clang AST transformation virtualizes PostgreSQL process-local state, and a
private C runtime supplies instance, role, session, signal, wait, path, and
execution ownership. The generated kernel powers three runtime modes:

| Mode | Runtime boundary | Lifecycle |
| --- | --- | --- |
| Embedded | Python or C API inside the application process | The application opens and closes the instance |
| Server | PostgreSQL protocol into a standalone threaded server | `initdb`, `postgres`, and `pg_ctl` |
| Browser | JavaScript API and WebAssembly in browser Workers | The browser SDK opens and closes the instance |

The [server](../server/index.md) is available as a source build; the
[browser package](../getting-started/browser.md) is a preview. The following
diagram describes the native embedded SDKs and their stable public ABI.

```text
Host application
    │
    ├── Python API or pgm_* C ABI
    │
    ├── postgamma lifecycle, executor, and memory transport
    │
    └── AST-generated PostgreSQL 19 kernel
            ├── SQL and catalogs
            ├── planner and executor
            ├── WAL and recovery
            └── reviewed bundled extensions
```

## Embedded protocol boundary

In embedded mode, frontend protocol semantics are preserved through a bounded
in-memory transport. Private libpq code implements PostgreSQL simple and extended
protocol behavior without creating an operating-system socket. The public API
does not expose libpq or PostgreSQL types.

## Embedded roles and pooled sessions

Long-lived PostgreSQL roles execute as host-safe threads rather than child
processes. Logical client sessions are scheduled onto a bounded set of carrier
workers. A session re-enters PostgreSQL at a request boundary; it does not move
a live C stack between workers.

Session state, role state, and instance state have separate generated ownership
stores. This is what permits concurrent connections and multiple independent
instances without turning every PostgreSQL global into one process-wide value.

## Stable embedded ABI

The host-facing ABI uses opaque handles, fixed-width scalar types, explicit
structure sizes, capability negotiation, and caller-owned diagnostics. It does
not include PostgreSQL headers. The Python binding consumes only this ABI and
does not reach into PostgreSQL implementation symbols.

## Standalone server boundary

The threaded server accepts PostgreSQL clients over TCP or Unix sockets.
Applications can use their existing PostgreSQL drivers. The server owns the
cluster directory, and its backend and background roles run as threads in that
server process. It is managed through PostgreSQL server tools rather than the
embedded API.

The embedded executor's worker defaults and API restrictions do not define the
standalone server's contract. See [server validation and compatibility](../server/compatibility.md)
for the source-build boundary, extensions, platform requirements, and release
work still needed.

## Browser boundary

The browser package runs the threaded kernel through WebAssembly and Workers.
It starts in memory by default and offers explicit OPFS persistence. The live
psql demo connects to this browser-local kernel. Browser isolation requirements,
storage ownership, and resource limits are described in the
[browser guide](../getting-started/browser.md).

## Upgrades

PostgreSQL-specific source identities and integration seams live in versioned
manifests. Upgrade audits compare a candidate upstream tree without changing
the supported product version. PostgreSQL 19 is the only current product
contract; candidate-major results measure adaptation work rather than support.
