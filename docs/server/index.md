# Threaded PostgreSQL server

PostGamma can run as a standalone PostgreSQL 19 server. PostgreSQL backend and
background roles run as threads within the server process. Applications connect
through the PostgreSQL protocol using psql or a database driver.

**Availability: source build.** The repository provides a server build and
threaded-runtime validation. A separate, release-qualified server binary package
is not yet available. The Python wheel and static C SDK are embedded products;
they do not install this server.

## Choose a runtime

| Mode | Database owner | Application interface | Start here |
| --- | --- | --- | --- |
| Embedded | Your application process | Python or the public C ABI | [Python](../getting-started/index.md), [C](../getting-started/c-static.md) |
| Server | A standalone threaded server process | PostgreSQL clients over TCP or a Unix socket | [Server quickstart](quickstart.md) |
| Browser | Browser Workers | JavaScript SDK or the live psql demo | [Browser preview](../getting-started/browser.md) |

Choose the server mode when separate applications need to connect to the same
running database. Clients share the server, rather than opening its data
directory themselves. The server remains the sole owner of that directory.

Choose embedded mode when the application should create, query, and close its
own database through an API. Choose the browser mode for local execution in a
web application. The homepage's live psql runs in the browser.

## What changes with threads

SQL sessions retain PostgreSQL session and transaction semantics, while the
runtime supplies thread-local role state and virtualized process operations.
Server threads share an operating-system process, its resource limits, and its
failure boundary. PostgreSQL process assumptions therefore matter when adapting
extensions and operational tooling.

Threading describes the execution model; performance and capacity must be
measured for the target workload. See [validation and compatibility](compatibility.md)
for the current delivery boundary.

## Continue

- [Build, initialize, and connect](quickstart.md)
- [Configure, stop, and back up a server](operations.md)
- [Validate a build and review compatibility](compatibility.md)
