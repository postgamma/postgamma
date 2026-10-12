# Server validation and compatibility

The threaded server is available as a source build over the pinned PostgreSQL 19
tree. There is no separate release-qualified server binary distribution yet.
The embedded SDK's release matrix and the browser preview's acceptance results
do not qualify a standalone server package.

## Validate the threaded runtime

The build target compiles the generated tree. To validate the transformation,
PostgreSQL behavior, sanitizers, and thread isolation, run:

```bash
make threaded-postgresql-check JOBS=4
```

When using the quickstart's custom install prefix, preserve it with
`PG_GENERATED_CONFIGURE_ARGS="--prefix=$POSTGAMMA_SERVER_PREFIX"` on the command.
The [installation guide](../getting-started/installation.md) lists the full-test
dependencies and process limits. This is an extensive validation suite, not a
startup smoke test.

The focused `make generated-thread-runtime-check JOBS=4` target installs a
temporary server, initializes a cluster, starts it through `pg_ctl`, queries it
through PostgreSQL clients, and checks thread/process evidence. It is useful for
runtime development and does not replace the full gate.

## Keep the support boundaries separate

| Area | Threaded server source build |
| --- | --- |
| Clients | PostgreSQL protocol over TCP or Unix sockets; use psql or a PostgreSQL driver |
| Lifecycle | `initdb`, `postgres`, and `pg_ctl`; the server owns its cluster directory |
| Threading | Backend and background roles share one process; process limits and native failures affect that process |
| Extensions | Validate against the transformed server and its threading model; an ordinary PostgreSQL extension build is not sufficient evidence |
| Logical backups | Use the matching `pg_dump` and `pg_restore` tools and verify a restore |
| Replication and physical backup | Require server-specific integration and recovery evidence for the chosen build and deployment |
| Platform | Depends on the source build's compiler, libc, libraries, and configure options; the wheel's glibc baseline does not apply automatically |
| Upgrades | Retain logical backups; direct cluster reuse across alpha builds or runtime modes is not promised |

The Python/C [unsupported-feature list](../compatibility/limits.md) describes the
embedded API. It is not the server's feature list. Equally, the presence of an
upstream server command or extension does not establish a supported PostGamma
deployment contract. The embedded product's bundled pgvector is not automatically
installed by the standalone server build.

## Before publishing a server package

A server distribution needs its own artifact manifest, checksums, dependency and
platform policy, and tests against the exact packaged bytes. Acceptance must
cover installation outside the source tree, startup and shutdown, authenticated
client connections, concurrent sessions, parallel queries, crash recovery, and
backup/restore. Advertise extensions and replication configurations only with
their corresponding server evidence.

See [downloads](../downloads.md) for current artifact availability and
[versioning](../releases/versioning.md) for source and release identities.
