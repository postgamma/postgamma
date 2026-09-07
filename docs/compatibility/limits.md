# Limits and unsupported features

## Default resource limits

| Limit | Default | Override |
| --- | ---: | --- |
| Materialized result | 16 MiB | `Database(result_buffer_limit=...)` |
| Individual value | 8 MiB | `Database(maximum_value_size=...)` |
| Executor workers | 4 | `Database(worker_count=...)` |
| Instance event queue | 64 events | `Database(event_queue_capacity=...)` |
| C transport queue | 64 KiB | `pgm_instance_options.transport_queue_capacity` |
| Logical archive channel | 256 KiB | dump/restore options |

A zero C or Python result-size option selects the embedded default; it does not
mean unbounded memory.

## Capacity rules

- One connection permits one in-flight request.
- Ordinary operations on one connection are serialized.
- Open transactions retain workers; simultaneously open transactions must not
  exceed the worker count.
- One host thread owns request or operation progress at a time.
- Logical dump and restore are serialized across the process.
- Event queue overflow is reported, not silently hidden.

## Not currently supported

- PostgreSQL pipeline mode;
- COPY BOTH and replication protocol;
- shell-backed `COPY ... PROGRAM` and embedded `system()` / `popen()` calls;
- raw pgwire access from the public API;
- physical backup and restore;
- parallel `pg_dump` or `pg_restore` workers;
- arbitrary native extensions loaded from unbundled shared objects;
- PostgreSQL background-worker extensions under SDK ABI v1;
- C transaction callback helpers;
- source distributions for the Python package;
- a prebuilt shared C library as a first-release artifact;
- non-Linux or non-x86-64 public wheel policies;
- built-in encryption at rest for cluster directories or logical archives.

An unsupported capability must return `PGM_STATUS_UNSUPPORTED` in C or raise
`NotSupportedError` in Python. PostGamma does not silently switch to a socket,
subprocess, temporary archive file, or weaker SQL behavior.

The absence of shell execution is not a SQL sandbox. The initial role is a
PostgreSQL superuser, and server-side file operations use the host process's
filesystem permissions. See the [security model](../concepts/security.md).
