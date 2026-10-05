# Connections and concurrency

One `Database` owns an instance and a bounded executor. One `Connection` owns a
logical PostgreSQL session. Separate connections may execute concurrently;
ordinary operations on the same connection are serialized.

Top-level `connect(path)` and `connect_async(path)` use a process-local registry.
Calls resolving to the same canonical path share one implicit `Database` while
returning independent sessions. The last such connection to close shuts down
the instance. Construct `Database` directly when instance ownership must be
explicit.

## Worker count

`worker_count` controls the maximum number of carrier workers that can execute
client sessions at once:

```python
database = postgamma.Database("agent.pgm", worker_count=8)
```

You may open more connections than workers. Idle and unpinned connections do
not permanently consume a worker. A connection retains a carrier while
PostgreSQL semantics require backend-local continuity, including an explicit
transaction.

`Connection.transaction()` and `AsyncConnection.transaction()` delimit a
transaction without closing the connection. A scope must begin while its
connection is idle, cannot be nested on that connection, commits on clean exit,
and rolls back when its body raises.

!!! danger "Pinned transaction limit"

    The number of simultaneously open transactions must not exceed
    `worker_count`. If all workers are retained by transactions, another
    request cannot make progress until a transaction commits or rolls back.

Inspect the current scheduling state with `Connection.status`:

```python
status = connection.status
print(status.pinned, status.pin_reasons, status.carrier_retained)
```

## Python threads

The module reports DB-API `threadsafety = 2`: threads may share the module and
connections, but callers should not concurrently mutate or execute through the
same connection. Blocking native execution releases the GIL, so separate
connections can run PostgreSQL work in parallel.

Cancellation is the documented exception: another thread may request
cancellation for the active connection or request. Closing an object must not
race another operation using the same object.

## Multiple instances

Multiple `Database` objects may remain live at once when they use different
cluster directories. Each instance owns its shared state, resource accounting,
event queue, and executor. Logical dump and restore operations are currently
serialized across the process even when they target different instances.

## Fork

An inherited live database handle is invalid in a forked child. Open lazily
after the fork, or close before the host forks:

```python
database.close_before_fork()
```

The child must construct and open a new `Database`. PostGamma never attempts to
repair inherited PostgreSQL threads, locks, or memory mappings after `fork()`.
