# Transactions

PostGamma preserves PostgreSQL transaction semantics. Python adds a DB-API
transaction policy and explicit scopes around the same logical session.

## Default DB-API behavior

Connections default to `autocommit=False`. The first non-transaction-control
statement starts a transaction automatically. Call `commit()` or `rollback()`
to release it:

```python
with database.connect() as connection:
    connection.execute("insert into jobs values ($1, $2)", [1, "ready"])
    connection.commit()
```

The connection context manager commits a successful body and rolls back a body
that raises. It then closes the connection.

## Scoped transactions on a long-lived connection

`transaction()` commits or rolls back one unit of work without closing the
connection:

```python
connection = postgamma.connect(
    "agent.pgm", mode=postgamma.OpenMode.OPEN_EXISTING
)
try:
    with connection.transaction() as transaction:
        transaction.execute(
            "insert into jobs values ($1, $2)", [1, "ready"]
        )
finally:
    connection.close()
```

A normal exit commits. If the body raises, PostGamma rolls back and preserves
the original exception. A scope must begin on an idle connection, is single-use,
and cannot be nested on the same connection. While it is active, let the scope
own commit and rollback.

The asynchronous form is symmetric:

```python
async with connection.transaction() as transaction:
    await transaction.execute(
        "update jobs set state = $1 where id = $2", ["done", 1]
    )
```

## Autocommit

Use autocommit for independent statements:

```python
with database.connect(autocommit=True) as connection:
    connection.execute("vacuum analyze jobs")
```

`autocommit` cannot change while a transaction is active. Commit or roll back
first.

## Capacity

An explicit or implicit open transaction pins its logical session to one
carrier worker. Keep transaction scopes short, and configure enough workers for
the maximum number of concurrently open transactions. Open transactions must
not exceed the instance worker count.

## Errors

PostgreSQL transaction errors retain ordinary PostgreSQL behavior. After a
statement fails inside a transaction, roll back before issuing unrelated work.
`Connection.transaction_status` and `Connection.status` expose the current
state without changing it.
