# Streaming and COPY

## Chunked rows

Set `pgm_execute_options.delivery_mode` to `PGM_DELIVERY_CHUNKED` and provide a
positive `target_chunk_rows`. Each `pgm_result` owns one bounded tuple chunk.

Column and value accessors return borrowed views:

```c
pgm_value_view value = PGM_VALUE_VIEW_INIT;
status = pgm_result_value(result, row, column, &value, &error);
if (status == PGM_STATUS_OK && !value.is_null)
    consume(value.type_oid, value.format, value.data, value.size);
```

The view expires when the result is freed. A late PostgreSQL error can appear
after earlier chunks, so continue the request sequence until its terminal
boundary.

## COPY IN

When a result reports `PGM_RESULT_COPY_IN`, transfer the stream handle with
`pgm_result_take_copy()`. `pgm_copy_write()` may consume only part of the input
and reports `PGM_IO_AGAIN` when progress requires servicing the opposite
transport direction. Call `pgm_copy_finish()` after all bytes are accepted.

## COPY OUT

For `PGM_RESULT_COPY_OUT`, call `pgm_copy_read()` repeatedly. It reports partial
output, `PGM_IO_AGAIN`, or `PGM_IO_END`. Continue the parent request after COPY
ends to receive PostgreSQL's command result and terminal status.

## Duplex progress rule

Never implement COPY as a blocking write-only loop. PostgreSQL may emit notices,
errors, and protocol control messages while the host is providing input. A host
waiting for input capacity must remain able to drain output and drive request
progress. This duplex rule prevents bounded queue self-deadlock.

## Abort and cleanup

Use `pgm_copy_abort()` to report a host-side input failure. Use
`pgm_copy_close()` for bounded stream retirement. The COPY handle, result, and
request must all close before the connection closes.

COPY BOTH and replication-protocol streaming are outside ABI v1.3.
