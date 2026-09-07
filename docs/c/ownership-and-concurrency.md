# Ownership and concurrency

## Structure initialization

Initialize every public input and output structure with its matching macro:

```c
pgm_instance_options options = PGM_INSTANCE_OPTIONS_INIT;
pgm_connection_options connection_options = PGM_CONNECTION_OPTIONS_INIT;
pgm_parameter parameter = PGM_PARAMETER_INIT;
pgm_value_view value = PGM_VALUE_VIEW_INIT;
```

Every extensible structure starts with `struct_size`. ABI v1 rejects a smaller
structure and ignores trailing bytes from a larger compatible caller.

## Ownership rules

- A successful open or create function returns one caller-owned handle.
- A close function consumes its handle only when it returns
  `PGM_STATUS_OK`; a failed close may be retried.
- `*_free()` functions consume their owned value and must not race another
  operation using it.
- Result values, column names, parameter-status values, event fields, and error
  strings are borrowed views. Copy them before releasing their owner.
- A `pgm_result` transfers a COPY handle only through
  `pgm_result_take_copy()`.

## Error ownership

Functions accepting `pgm_error **` clear the output before work begins. On a
non-OK return, they attempt to return one owned diagnostic whose status matches
the returned status. Diagnostic allocation failure is the only reason a caller
that supplied the output may still receive `NULL`.

```c
pgm_error *error = NULL;
pgm_status status = pgm_connection_open(
    instance, &options, &connection, &error);
if (status != PGM_STATUS_OK)
{
    log_error(
        pgm_status_name(status),
        error ? pgm_error_sqlstate(error) : NULL,
        error ? pgm_error_message(error) : NULL);
    pgm_error_free(error);
}
```

## Thread-safety matrix

| Object | Concurrent use |
| --- | --- |
| Library metadata | Safe from any host thread |
| Different instances | May operate concurrently |
| Different connections | May execute concurrently |
| Same connection | Serialize mutation and execution |
| Active request | One progress/wait owner; one documented canceling thread |
| COPY handle | One I/O owner; cancellation may come from another thread |
| Operation | One progress/wait owner; one cancellation thread |
| Event queue | Choose one consumer model per instance |

An instance has a bounded number of executor workers. A session retains a
worker during an explicit transaction or other advertised pin condition. The
number of simultaneously executing pinned transactions must not exceed
`executor_worker_count`.

## Callbacks

Log, notice, notification, and logical-stream callbacks run only on the host
thread performing progress or dispatch. They never run on a PostgreSQL role
thread or while an internal PostGamma lock is held. Reentry into the same
instance from such a callback returns `PGM_STATUS_REENTRANT_CALL`.

Callback arguments are borrowed for the callback duration.

## Fork

Do not use inherited handles after `fork()`. The child receives
`PGM_STATUS_FORKED_PROCESS` and must open a fresh instance. Quiesce and close all
PostGamma objects before forking when the host controls the lifecycle.
