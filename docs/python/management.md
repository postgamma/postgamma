# Management and logical backup

Checkpoint, maintenance, logical dump, and logical restore run in process and
return `Operation` progress when `wait=False`.

## Checkpoint and maintenance

```python
database.checkpoint()
database.maintenance("vacuum_analyze")
database.maintenance("reindex_database", database="postgres")
```

To observe progress yourself:

```python
operation = database.checkpoint(wait=False)
try:
    progress = operation.wait(timeout=30)
    print(progress.state, progress.phase)
finally:
    operation.close()
```

## Logical dump and restore

```python
--8<-- "examples/python/backup_restore.py"
```

Run the example in an empty directory. It prints `Restored rows: 1` after the
archive has been consumed by a separately initialized cluster.

The archive is PostgreSQL's custom logical archive carried through bounded
in-memory callbacks. PostGamma does not launch `pg_dump` or `pg_restore` as a
subprocess and does not create a staging archive file.

Supported flags are exposed by `LogicalFlags`, including schema-only,
data-only, clean, create, no-owner, and no-privileges modes. Schema-only and
data-only are mutually exclusive.

The stream must provide `write()` for dump or `read()` for restore. Partial
transfers and ordinary temporary unavailability are handled by the caller-driven
operation. A callback exception becomes the operation's causal error.

## Current serialization rule

Only one logical dump or restore operation may own the frontend-tool runtime in
the process at a time. Another submission returns a busy error, including when
it belongs to another database instance. Checkpoints and ordinary SQL do not
share this global logical-tool admission slot.

## Major-version migration

Physical database directories are PostgreSQL-major-specific. Logical dump from
the old product and logical restore into a new instance is the supported
migration primitive. An automated cross-major migration command is not part of
the current release.
