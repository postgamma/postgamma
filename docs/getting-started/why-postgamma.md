# When to use PostGamma

PostGamma is for applications that need PostgreSQL behavior but do not want to
install, configure, or supervise a database server. It runs PostgreSQL 19 in
the application process and exposes Python and C APIs over an in-memory
protocol transport.

## Choose PostGamma when

- existing code depends on PostgreSQL SQL, types, transactions, SQLSTATEs, or
  catalog behavior;
- an agent, CLI, desktop application, test harness, or local service should own
  its database lifecycle;
- several sessions must execute concurrently inside one operating-system
  process;
- a directory-based PostgreSQL cluster is an acceptable storage layout; and
- every native extension can be reviewed and bundled with the product build.

PostGamma is particularly useful when replacing PostgreSQL with another SQL
dialect would create more application work than embedding the PostgreSQL
kernel.

## Do not choose PostGamma when

!!! danger "One read-write owner process"

    One cluster directory has exactly one live operating-system process owner.
    Several sessions and threads may share it inside that process, but another
    process cannot open the same cluster at the same time.

Choose another design when you require:

- several operating-system processes to open the same database directly;
- a single-file database artifact;
- the smallest possible binary and idle-memory footprint;
- browser or JavaScript WebAssembly deployment; or
- an analytics-first columnar engine and Arrow-native execution.

A normal PostgreSQL server remains the right answer when independent processes,
machines, or untrusted clients need concurrent access through a network
boundary.

## Compare the application boundaries

| Choice | SQL and execution focus | Persistence | Concurrent access boundary | Extension model |
| --- | --- | --- | --- | --- |
| PostGamma | PostgreSQL 19 OLTP and general SQL | PostgreSQL cluster directory | Multiple sessions in one owner process | Reviewed extensions linked into the build |
| SQLite | Compact SQLite SQL engine | Usually one database file | Multiple processes may read; writers serialize | SQLite loadable or compiled-in extensions |
| DuckDB | Analytical SQL and columnar execution | One database file or memory | Concurrent work in one read-write process | DuckDB extension ecosystem |
| PGlite | PostgreSQL compiled to WebAssembly | Memory, browser storage, or supported JavaScript filesystems | JavaScript/WASM application boundary | Extensions ported and packaged for PGlite |
| PostgreSQL server | Full PostgreSQL server | PostgreSQL cluster directory | Independent local or network clients | Normal PostgreSQL extensions |

This table compares product boundaries, not benchmark results. Review the
upstream documentation for [SQLite's intended uses](https://www.sqlite.org/whentouse.html),
[DuckDB concurrency](https://duckdb.org/docs/stable/connect/concurrency), and
[PGlite's WASM model](https://pglite.dev/docs/about) when those boundaries are
closer to the application you are building.

## What PostGamma refuses to hide

PostGamma does not silently start a server, open a socket, launch a frontend
tool, write a temporary logical archive, guess an unknown PostgreSQL type, or
load an unreviewed native extension. Unsupported behavior returns an explicit C
status or Python exception.

That contract is more important than API similarity with another embedded
database: PostgreSQL behavior wins, and a missing capability fails visibly.

## Continue

- [Install the Python wheel or static C SDK](installation.md)
- [Create the first cluster](index.md)
- [Understand concurrency](../concepts/concurrency.md)
- [Review limits](../compatibility/limits.md)
