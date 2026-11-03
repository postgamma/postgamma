# Get started with PostGamma

PostGamma makes PostgreSQL 19 available inside applications and agents, as a
standalone threaded server, and in the browser. Choose how the database runs,
then follow the quickstart for that environment.

## What we built

PostGamma adapts PostgreSQL's process-oriented runtime so database roles can
execute as threads. PostgreSQL still supplies the parser, planner, executor,
SQL types, transactions, WAL, and recovery. PostGamma supplies the runtime and
lifecycle boundaries around that core:

- **A threaded PostgreSQL core.** Source transformation and a C runtime manage
  the state and process operations that PostgreSQL roles need when running as
  threads.
- **Native embedding APIs.** A stable C ABI and Python package let an application
  open a database, use independent sessions, and shut it down inside its own
  process.
- **A browser runtime.** WebAssembly and Workers bring the kernel to web
  applications through a JavaScript SDK, with memory storage and optional OPFS
  persistence.

The threaded core can also run as a standalone server for PostgreSQL clients.
The modes share a PostgreSQL foundation, while their interfaces, storage
ownership, and supported features have separate contracts. The
[architecture guide](../concepts/architecture.md) explains those boundaries.

## Choose your path { #choose-your-path }

### Embedded: Python or C

Choose this when an app, agent, CLI, or native service should own its database
lifecycle. The database runs inside the application process; no separate SQL
server is required.

- [Python quickstart](../getting-started/index.md): create a persistent cluster,
  insert and query JSON data, then close and reopen it.
- [Static C quickstart](../getting-started/c-static.md): link the SDK, run a
  parameterized query, checkpoint, and shut down the instance.

The Python wheel and static C SDK are unreleased alpha candidates. Start with
the [source build instructions](../getting-started/installation.md), and check
[downloads](../downloads.md) for binary availability.

### Server: PostgreSQL clients

Choose this when independent applications should connect to one database
service. A standalone server owns the cluster, and its PostgreSQL roles run as
threads. Clients connect using psql or a PostgreSQL driver.

[Server quickstart](../server/quickstart.md): build and install the server,
initialize a cluster, connect with psql, and stop it cleanly.

Available as a **source build**. A separate server binary package is not yet
available. Review the [server support boundary](../server/compatibility.md) when
choosing a deployment.

### Browser: JavaScript and WebAssembly

Choose this when SQL and data should stay in a web application. The database
runs in browser Workers. Start in memory, or explicitly choose OPFS storage
when data should survive a reload.

[Browser quickstart](../getting-started/browser.md): build and install the package,
serve its assets, open a database, and execute a query through JavaScript.

Available as a **source-built preview**. The guide covers HTTPS, browser
isolation, storage ownership, and resource limits.

## Try your first SQL query

For an immediate look at the PostgreSQL engine, open the
[live psql terminal](../index.md#live-demo). It starts automatically in your browser
and requires no local installation. Enter:

```sql
SELECT version();
SELECT 6 * 7 AS answer;
```

The second query returns `42`. The demo uses an in-memory database for this
visit. Use the quickstart for your chosen mode to integrate the database and
configure persistent storage.

## Where to go next

- [When to use PostGamma](../getting-started/why-postgamma.md): decide whether the
  application and database boundaries fit your use case.
- [Python guides](../python/index.md) and [C SDK guides](../c/index.md): work with
  sessions, transactions, COPY, events, and logical backup in embedded mode.
- [Server operations](../server/operations.md): configure, manage, and back up a
  standalone server.
- [Browser guide](../getting-started/browser.md): configure browser persistence,
  hosting, and the JavaScript API.
- [Downloads](../downloads.md): check artifact availability, platforms, and
  verification instructions.
