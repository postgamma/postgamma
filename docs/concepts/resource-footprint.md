# Resource footprint

PostGamma embeds a PostgreSQL kernel, so its startup cost and idle footprint are
larger than a compact single-file engine. Size the product before choosing it
for a constrained agent, CLI, or desktop application.

## Default resource profile

| Resource | Embedded default |
| --- | ---: |
| PostgreSQL `shared_buffers` | 16 MiB |
| PostgreSQL `max_connections` | 8 |
| PostGamma carrier workers | 4 |
| PostgreSQL `max_worker_processes` | 4 |
| PostgreSQL `max_parallel_workers` | 2 |
| Result materialization limit | 16 MiB |
| Individual value limit | 8 MiB |
| Instance event queue | 64 events |

`worker_count` controls concurrently executing client sessions. Increasing it
adds carrier-thread stacks and permits more pinned transactions; it does not
change PostgreSQL `max_connections` automatically. Configure both deliberately
when increasing concurrency.

## Checked startup budget

The Linux release gate measures the process before and after real PostgreSQL
startup and rejects a build outside these current budgets:

| Measurement | Release-gate ceiling |
| --- | ---: |
| Cold open to ready | 5 seconds |
| Warm open to ready | 3 seconds |
| Crash-recovery open to ready | 8 seconds |
| Peak resident-memory delta | 96 MiB |
| Peak additional threads | 12 |
| Peak additional file descriptors | 32 |

These are regression ceilings, not latency promises. Actual values depend on
the CPU, filesystem, libc, extension set, configuration, and whether recovery
is required. Release artifacts should publish their measured wheel, static SDK,
and resource-pack sizes beside the artifacts rather than treating a developer
build as universal.

## One checked development build

The 2026-08-29 debug-and-assertion build on x86-64 Linux 6.8, glibc 2.39, and an
Intel Xeon Gold 6152 produced this sample:

| Measurement | Observed value |
| --- | ---: |
| Cold open to ready | 222 ms |
| Warm open to ready | 188 ms |
| Crash-recovery open to ready | 762 ms |
| Peak resident-memory delta | 24.3 MiB |
| Peak additional threads | 10 |
| Peak additional file descriptors | 23 |
| CPython 3.12 wheel, compressed | about 12 MiB |
| Static library before the final application link | about 26 MiB |
| Runtime resource pack | about 17 MiB |

The lifecycle values come from the checked Linux `procfs` receipt; the artifact
sizes come from the same source checkout. They are feasibility examples, not a
release minimum, maximum, or cross-machine benchmark. Optimized manylinux
release artifacts must publish their own values.

## Thread stacks and virtual memory

Dedicated PostgreSQL roles use explicit 4 MiB stacks. Client carriers use 8 MiB
stacks, with guard pages, so virtual address-space growth can be much larger
than resident memory. On 64-bit Linux this is normally address-space
reservation rather than committed RAM, but container and `ulimit` policies may
still constrain it.

## Reduce application memory pressure

- Keep the default worker count unless measured concurrency needs more.
- Stream large row sets instead of increasing the materialized-result limit.
- Use COPY for bulk transfer.
- Close transactions promptly so workers can serve another session.
- Open one managed `Database` and reuse sessions instead of repeatedly starting
  independent clusters.
- Measure with the actual extension and GUC profile shipped by the application.

See [Connections and concurrency](concurrency.md) and
[Limits and unsupported features](../compatibility/limits.md).
