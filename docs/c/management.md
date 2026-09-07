# Management and logical backup

Management APIs return a `pgm_operation` driven by the same level-triggered
waitable model as requests.

## Checkpoint

```c
pgm_checkpoint_options options = PGM_CHECKPOINT_OPTIONS_INIT;
pgm_operation *operation = NULL;

status = pgm_instance_checkpoint_async(
    instance, &options, &operation, &error);
```

Call `pgm_operation_progress()` once to submit pending work, wait on the stable
descriptor from `pgm_operation_waitable()`, and recheck until the operation is
completed, canceled, or failed. A dispatched checkpoint is not cancelable.

## Maintenance

`pgm_instance_maintenance_async()` supports VACUUM, ANALYZE, VACUUM ANALYZE,
and database REINDEX. It uses a private in-memory SQL connection and does not
open a network endpoint.

## Logical dump and restore

`pgm_instance_logical_dump_async()` writes a PostgreSQL custom archive through
`pgm_stream_write_callback`. `pgm_instance_logical_restore_async()` consumes
one through `pgm_stream_read_callback`.

Callbacks may transfer fewer bytes than offered or return `PGM_IO_AGAIN`. They
run only on the host thread calling `pgm_operation_progress()`. The operation
does not create an archive staging file or launch a frontend-tool subprocess.

Only one logical dump or restore owns the logical frontend-tool runtime across
the process. A second submission returns `PGM_STATUS_BUSY`, even when it belongs
to another instance.

## Progress and cancellation

`pgm_operation_get_progress()` reports operation kind, state, phase, bytes, and
object counters. Totals are zero when no estimate is available. One thread owns
progress and wait calls; one other thread may call `pgm_operation_cancel()`.
`pgm_operation_free()` must not race either thread.

Logical dump and restore are the supported PostgreSQL-major migration
primitive. Physical backup and restore are not part of the current ABI.
