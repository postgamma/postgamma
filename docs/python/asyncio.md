# Asyncio

The asyncio API drives PostGamma's level-triggered waitable descriptors from
the active event loop. Query progress does not allocate one Python helper thread
per request. Instance startup and shutdown may use bounded thread offload because
those lifecycle operations are blocking.

```python
--8<-- "examples/python/async_concurrency.py"
```

Run the example in an empty directory. It prints `(0,) (42,)`; the two result
objects are produced by independent logical sessions scheduled through one
embedded instance.

## One asynchronous connection

`connect_async()` is the direct counterpart of `connect()`. Awaiting it returns
an already-open `AsyncConnection`:

```python
connection = await postgamma.connect_async("agent.pgm", autocommit=True)
try:
    result = await connection.execute("select current_database()")
    print(result.fetchone())
finally:
    await connection.close()
```

It accepts the same path, mode, logical database, role, settings, worker count,
autocommit, and timeout options as synchronous `connect()`. Sync and async
top-level calls for the same canonical path share the same process-local
instance registry and still return independent sessions.

## Explicit async instance

Use `AsyncDatabase` for several connections, events, checkpoint, maintenance,
or logical backup and restore:

```python
async with postgamma.AsyncDatabase("agent.pgm", worker_count=4) as database:
    first = await database.connect(autocommit=True)
    second = await database.connect(autocommit=True)
    try:
        left, right = await asyncio.gather(
            first.execute("select pg_sleep(0.1), 1"),
            second.execute("select pg_sleep(0.1), 2"),
        )
    finally:
        await first.close()
        await second.close()
```

`AsyncDatabase` may wrap an existing synchronous `Database` through
`database.as_async()`. `AsyncConnection` similarly wraps a logical session
through `connection.as_async()`.

## Concurrency

Use separate connections for simultaneous work. One logical connection retains
the same serialization rule in synchronous and asynchronous code:

```python
left, right = await asyncio.gather(
    first.execute("select pg_sleep(0.1), 1"),
    second.execute("select pg_sleep(0.1), 2"),
)
```

The number of simultaneously open transactions must not exceed the database's
worker count.

## Transaction scopes

An async transaction scope has the same commit and rollback rules as its sync
counterpart and does not close the connection:

```python
connection = await postgamma.connect_async(
    "agent.pgm",
    mode=postgamma.OpenMode.OPEN_EXISTING,
)
try:
    async with connection.transaction() as transaction:
        await transaction.execute(
            "insert into jobs(id, state) values ($1, $2)",
            [1, "queued"],
        )
finally:
    await connection.close()
```

A clean scope commits and an exception rolls back. Nested scopes on the same
connection are rejected.

## Cancellation and timeouts

Canceling an asyncio task routes cancellation to the PostgreSQL request, retires
the native request, and leaves the connection reusable when PostgreSQL can
recover it safely. Query deadlines use the same cancellation path and raise
`QueryTimeoutError`.

```python
task = asyncio.create_task(connection.execute("select pg_sleep(30)"))
task.cancel()
try:
    await task
except asyncio.CancelledError:
    pass
```

Do not close a connection concurrently with another operation using it. Await
the canceled task before closing the connection.

## Events

`AsyncDatabase.events()` is an async iterator over the instance event queue:

```python
async for event in database.events():
    print(event.kind, event.connection_id, event)
```

The iterator stops when the database closes and does not retain a closed event
loop.
