# Architecture

PostGamma treats an unmodified PostgreSQL source tree as compiler input. A
Clang AST transformation virtualizes PostgreSQL process-local state, and a
private C runtime supplies instance, role, session, signal, wait, path, and
execution ownership. The generated kernel is linked behind a stable public ABI.

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

## No server protocol endpoint

Frontend protocol semantics are preserved through a bounded in-memory
transport. Private libpq code implements PostgreSQL simple and extended
protocol behavior without creating an operating-system socket. The public API
does not expose libpq or PostgreSQL types.

## Threaded roles, pooled sessions

Long-lived PostgreSQL roles execute as host-safe threads rather than child
processes. Logical client sessions are scheduled onto a bounded set of carrier
workers. A session re-enters PostgreSQL at a request boundary; it does not move
a live C stack between workers.

Session state, role state, and instance state have separate generated ownership
stores. This is what permits concurrent connections and multiple independent
instances without turning every PostgreSQL global into one process-wide value.

## Stable boundary

The host-facing ABI uses opaque handles, fixed-width scalar types, explicit
structure sizes, capability negotiation, and caller-owned diagnostics. It does
not include PostgreSQL headers. The Python binding consumes only this ABI and
does not reach into PostgreSQL implementation symbols.

## Upgrades

PostgreSQL-specific source identities and integration seams live in versioned
manifests. Upgrade audits compare a candidate upstream tree without changing
the supported product version. PostgreSQL 19 is the only current product
contract; candidate-major results measure adaptation work rather than support.
