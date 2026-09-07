# Durability and recovery

PostGamma uses PostgreSQL WAL, checkpoints, page-write protection, and crash
recovery. Removing the server process does not replace PostgreSQL's storage
engine with a weaker persistence layer.

## Default commit guarantee

Unless the application overrides them, the durability settings retain the
PostgreSQL defaults:

| Setting | Default | Meaning |
| --- | --- | --- |
| `fsync` | `on` | PostgreSQL asks the operating system to make required writes durable |
| `synchronous_commit` | `on` | A successful commit waits for the required WAL flush |
| `full_page_writes` | `on` | The first changed page after a checkpoint is protected against torn writes |
| `wal_sync_method` | Platform-selected | PostgreSQL selects the supported WAL synchronization method |

With these settings, returning successfully from `commit()` or a committed
autocommit statement has the same storage meaning as the corresponding local
PostgreSQL commit. The guarantee still depends on the operating system,
filesystem, and storage hardware honoring synchronization requests.

`settings={"synchronous_commit": "off"}` trades that acknowledgement guarantee
for throughput. A host or machine crash may then lose recently acknowledged
transactions. Changing `fsync` or `full_page_writes` weakens recovery safety and
should be treated as an application-level data-loss decision.

## Host termination

| Event | What happens |
| --- | --- |
| Normal `Database.close()` | Active handles must be released, PostgreSQL shuts down in process, and committed data remains |
| Unhandled Python exception after a committed transaction | The committed transaction remains subject to its commit settings |
| Host `SIGKILL`, `_exit()`, or power loss | No cleanup callback can run; the next open performs WAL recovery |
| Uncommitted transaction at host death | Recovery removes its effects |

The release gate commits one transaction, leaves another uncommitted, kills the
entire host with `SIGKILL`, reopens the cluster in a new host, and requires the
committed row to exist and the uncommitted row to be absent.

## Checkpoints

PostgreSQL's in-process checkpointer role performs normal periodic checkpoints.
Calling `Database.checkpoint()` or `pgm_instance_checkpoint_async()` is not
required after every commit. It is useful before an application-controlled
snapshot, before measuring recovery work, or when the host needs an explicit
management boundary.

```python
with postgamma.Database("agent.pgm") as database:
    database.checkpoint(wait=True)
```

A checkpoint is not a logical backup and does not make a cluster portable to a
different PostgreSQL major.

## Embedded storage defaults

The embedded profile reduces service-oriented background work: it uses
`shared_buffers=16MB`, `max_wal_size=64MB`, `wal_level=minimal`, and disables
archiving and autovacuum by default. Safety settings also disable server
listeners, crash-in-place restart, and external recovery commands.

Because autovacuum is off, applications with sustained updates or deletes must
schedule maintenance explicitly:

```python
with postgamma.Database("agent.pgm") as database:
    database.maintenance("vacuum_analyze")
```

## Back up before changing builds

Use an in-process logical dump before moving data to another PostgreSQL major
or to a PostGamma alpha whose release notes do not explicitly promise direct
cluster compatibility. See [Management and logical backup](../python/management.md)
and [PostgreSQL compatibility](../compatibility/postgresql.md).
