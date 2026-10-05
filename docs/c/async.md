# Asynchronous execution

The native asynchronous API is caller-driven. PostGamma exposes stable,
level-triggered waitable file descriptors so a host event loop can decide when
to call progress again.

## Submit

Use `pgm_execute_async_ex()` for typed parameters, delivery policy, result
format, per-request notice routing, and bounded result configuration:

```c
pgm_execute_options options = PGM_EXECUTE_OPTIONS_INIT;
pgm_request *request = NULL;
pgm_error *error = NULL;

options.parameters = parameters;
options.parameter_count = parameter_count;
options.delivery_mode = PGM_DELIVERY_CHUNKED;
options.target_chunk_rows = 512;

pgm_status status = pgm_execute_async_ex(
    connection, sql, strlen(sql), &options, &request, &error);
```

A connection permits one in-flight request. Submission copies the SQL and
parameter bytes required after the call returns.

## Progress loop

1. Call `pgm_request_progress()` without blocking.
2. Drain every ready result with `pgm_request_next_result()`.
3. If availability is `PGM_AVAILABILITY_AGAIN`, wait for the descriptor from
   `pgm_request_waitable()`.
4. Because the descriptor is level-triggered, recheck state after every wake.
5. Continue until `PGM_AVAILABILITY_END`, then free the request.

`pgm_request_wait()` is a bounded synchronous convenience for hosts that do not
need event-loop integration.

## Ordered results

`pgm_execute_script_async()` uses PostgreSQL simple-protocol semantics and may
produce multiple ordered command or tuple results. The caller must continue
until the terminal boundary even after consuming the result it expected.

Ordinary extended-protocol execution accepts one statement. Prepared statement
execution follows the same request and result-sequence model.

## Cancellation

`pgm_request_cancel()` routes a virtual PostgreSQL cancel to the owning request;
it does not send a process signal to the host. Another thread may request
cancellation while one thread owns progress. Wait for progress to reach a
terminal boundary before closing the connection.

Freeing an active request performs bounded retirement. If clean retirement is
impossible, PostGamma aborts that request transport and marks the connection
failed but still closable.
