# Bundled extension SDK

PostGamma supports reviewed native extensions compiled into the private kernel.
It does not load arbitrary PostgreSQL `.so` files at runtime.

Application developers using the bundled pgvector extension should start with
[Vector search with pgvector](pgvector.md). The rest of this page describes the
C contract for authors who adapt another native extension.

## Why extensions are bundled

An ordinary PostgreSQL extension may assume one backend process, process-global
mutable state, `dlopen` symbol visibility, server-owned signals, unrestricted
filesystem access, or background-worker process semantics. Those assumptions
are unsafe in an embedded multi-instance host.

Bundling makes every native module part of the link closure and requires an
explicit safety declaration before it can start.

## Descriptor contract

An extension provides one immutable `pgmex_descriptor` and registers it from
`_PG_init()`:

```c
#include <postgamma/postgamma_extension.h>

static const pgmex_descriptor descriptor = {
    .struct_size = sizeof(pgmex_descriptor),
    .abi_version = PGMEX_ABI_VERSION,
    .postgresql_major = 19,
    .capabilities =
        PGMEX_CAP_THREAD_SAFE |
        PGMEX_CAP_MULTI_INSTANCE_SAFE |
        PGMEX_CAP_SESSION_MOBILITY_SAFE |
        PGMEX_CAP_PARALLEL_WORKER_SAFE,
    .id = "example",
    .sql_name = "example",
    .version = "1.0",
};

void
_PG_init(void)
{
    if (pgmex_register(&descriptor) != 0)
        elog(ERROR, "could not register example extension");
}
```

The first four safety capabilities are reviewed claims:

- no unsynchronized process-global mutable state;
- complete instance isolation;
- logical-session state survives carrier changes;
- parallel-worker entry points do not require connection or session state.

## Lifecycle

The SDK distinguishes library initialization, instance shared-memory request,
instance startup, instance shutdown, session initialization, explicit session
reset, and session destruction. Extension state must live in the corresponding
instance or session storage rather than a hidden C global.

Parallel workers have instance identity but connection id zero and no logical
session state. An SQL function using session state must be declared
`PARALLEL RESTRICTED` or `PARALLEL UNSAFE`.

## Resources

Read-only resources must be declared in the extension build manifest and are
resolved relative to the installed resource pack. `pgmex_resource_read()`
rejects undeclared paths, dot components, and host-working-directory fallback.

Instance shared memory and filesystem reads require their matching capability
bits. Background workers, filesystem writes, host-library dependencies, and
process-global state are rejected by SDK ABI v1.

## Packaging

The extension object, control file, SQL files, and declared resources are
selected by a versioned build manifest. The build generates the static module
registry and validates symbols, state ownership, permissions, lifecycle hooks,
and package hashes before linking the kernel.
