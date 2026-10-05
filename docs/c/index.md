# C SDK overview

The PostGamma C SDK is a versioned, language-neutral interface to the embedded
kernel. The first release ships it as a static library with a matching runtime
resource pack.

[Build the first program](../getting-started/c-static.md){ .md-button .md-button--primary }
[Open the API reference](reference.md){ .md-button }

## Start with seven operations

A basic application needs only this lifecycle:

| Operation | Purpose | Releases with |
| --- | --- | --- |
| `pgm_instance_open()` | Open or create one cluster instance | `pgm_instance_close()` |
| `pgm_connection_open()` | Open one logical PostgreSQL session | `pgm_connection_close()` |
| `pgm_execute()` | Execute one parameterized statement | Returns `pgm_result` |
| `pgm_result_value()` | Borrow one cell from the result | Valid while result lives |
| `pgm_result_free()` | Release a materialized result | — |
| `pgm_connection_close()` | Close the logical session | — |
| `pgm_instance_close()` | Shut down the embedded instance | — |

The [static C quickstart](../getting-started/c-static.md) compiles and runs the
complete checked example. Begin there instead of reading the complete API
reference from top to bottom.

## Object hierarchy

```text
pgm_instance
├── pgm_connection
│   ├── pgm_statement
│   └── pgm_request
│       ├── pgm_result
│       └── pgm_copy
├── pgm_event
└── pgm_operation
```

An instance owns one running PostgreSQL cluster and its worker executor.
Connections are independent logical sessions. Statements, requests, results,
and COPY handles belong to one connection. Events and management operations
belong to the instance.

Close children before parents. A parent close fails rather than silently
destroying a live child handle.

## Stable host boundary

Host code includes:

```c
#include <postgamma/postgamma.h>
```

The header exposes opaque handles, fixed-width integers, sized public
structures, status codes, callbacks, and borrowed byte views. It does not
require PostgreSQL headers and does not expose `MemoryContext`, `Datum`, or
other PostgreSQL implementation types.

The static archive localizes private PostgreSQL symbols. The supported host
surface consists of the declared `pgm_*` symbols only.

## Check compatibility before opening

```c
if (pgm_abi_version() != PGM_ABI_VERSION)
    return incompatible_abi;

if (strcmp(pgm_postgresql_version(), "19") != 0)
    return incompatible_cluster_format;

if ((pgm_capabilities() & required_capabilities) != required_capabilities)
    return missing_feature;
```

Every optional surface has a capability bit. A consumer must handle
`PGM_STATUS_UNSUPPORTED` even when its development build advertised the
feature.

## Header roles

| Header | Audience | Purpose |
| --- | --- | --- |
| `postgamma/postgamma.h` | Every native host | Instances, connections, SQL, streaming, COPY, events, and management |
| `postgamma/postgamma_arrow.h` | Arrow consumers | Optional Arrow C Data result export |
| `postgamma/postgamma_extension.h` | Reviewed bundled extensions | Separate static extension ABI |

The extension header is not a host query API. It does not make arbitrary
PostgreSQL `.so` files safe to load into an application process.

## Continue by task

- [Build, link, and deploy](build-and-link.md)
- [Ownership and concurrency](ownership-and-concurrency.md)
- [Caller-driven asynchronous execution](async.md)
- [Streaming and COPY](streaming-and-copy.md)
- [Management and logical backup](management.md)
- [Complete C API reference](reference.md)

## Choosing an opening mode

ABI 1.4 adds `pgm_instance_open_with_mode()`. Initialize options with
`PGM_INSTANCE_OPTIONS_INIT`, set the paths and settings, and leave `create`
and `reserved` at zero:

```c
pgm_status status = pgm_instance_open_with_mode(
    &options, PGM_CREATE_NEW, &instance, &error);
```

| Mode | Missing path | Existing compatible cluster |
| --- | --- | --- |
| `PGM_OPEN_EXISTING` | Fails | Opens |
| `PGM_OPEN_OR_CREATE` | Atomically creates | Opens |
| `PGM_CREATE_NEW` | Atomically creates | Fails without replacing it |

`PGM_CREATE_NEW` also rejects an existing empty directory. A rejection because
the destination already exists returns `PGM_STATUS_INSTANCE_FAILED`; inspect
`pgm_error_detail(error)` for the create-new destination diagnostic. Invalid
options and lock failures retain their own status codes. Creation still uses
in-process initialization, directory locking and atomic publication. If startup
fails after publication, the valid cluster remains available for a later open.

Existing ABI 1.x callers can keep using `pgm_instance_open()` with `create=0`
(open existing) or `create=1` (open or create). The options structure layout,
old symbols and their ownership rules are unchanged.
