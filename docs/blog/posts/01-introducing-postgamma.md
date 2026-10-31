---
title: Introducing PostGamma
description: PostgreSQL with threads, as a server or embedded in C and Python applications.
author: Shujie Zhang
categories:
  - Announcements
draft: false
date: 2026-10-31T13:19:00+00:00
slug: introducing-postgamma
cover: assets/images/blog/introducing-postgamma.webp
cover_alt: A blue elephant drawn as a database workshop, with colorful connections to application windows.
---

# Introducing PostGamma

I've been working on PostGamma. It runs PostgreSQL with threads instead
of one process per connection.

You can use it to build a threaded PostgreSQL server, or embed PostgreSQL
directly in a C or Python application. When embedded, the database runs
inside your application process. You don't need a separate database server.

The embedded version supports concurrent connections and writes. It keeps
PostgreSQL's transactions, WAL, and crash recovery. It also includes pgvector.

The project is still an early alpha. You can
[install it from PyPI](https://pypi.org/project/postgamma/0.1.0a1/)
or [build it from source](https://postgamma.com/getting-started/installation/).

## Why I wanted this

PostgreSQL already has many things I want in a local application. It has a
SQL engine, transactions, JSONB, and extensions such as pgvector.

I wanted to use those features through a library. The application would open
the database, run queries, and close it. The database engine would run inside
the application's own process.

That is the goal of the embedded version of PostGamma. It uses PostgreSQL's
parser, planner, executor, and storage code. The application controls when
the database starts and stops.

This could be useful for a desktop tool, a local agent, or a test program.
These programs may need PostgreSQL features while keeping their data local.

## A small Python example

After building and installing the Python package, you can run this:

```python
import postgamma

with postgamma.connect("hello.pgm", autocommit=True) as db:
    db.execute(
        "create table if not exists notes ("
        "id bigint primary key, body text not null)"
    )
    db.execute(
        "insert into notes values ($1, $2) "
        "on conflict (id) do update set body = excluded.body",
        [1, "hello from PostGamma"],
    )
    row = db.execute(
        "select body from notes where id = $1", [1]
    ).fetchone()
    print(row)
```

The output is:

```text
('hello from PostGamma',)
```

`hello.pgm` is a PostgreSQL data directory. It holds the tables, catalogs,
WAL, and other database files. PostGamma creates it when it is missing.
The data stays there after the program closes.

C applications can use the
[static C SDK](https://postgamma.com/getting-started/c-static/).
Both interfaces use the same embedded PostgreSQL engine.

## How the project started

My first approach was to use AI coding tools to edit PostgreSQL directly.
After about 48 hours, I had an initial prototype. It compiled and passed
regression tests.

That was encouraging. But it left me with a maintenance problem. PostgreSQL
keeps changing. I would need to keep all of those edits working with new
upstream code.

So I changed the approach.

PostGamma now takes an unmodified PostgreSQL checkout as input. It uses
Clang to read the C code and generate source changes. A runtime library
provides the thread and state management.

The build applies the changes to a generated copy. The upstream checkout
stays untouched.

I explain this in more detail in
[Threading PostgreSQL Without Editing Upstream Source Code](02-threading-postgresql-without-editing-upstream-source-code.md).

## What took more work

Getting the prototype to compile was the beginning. Much of the later work
was about what happens during normal use and when something goes wrong.

Two connections need separate session state. Cancelling one query must
reach the right connection. Closing a connection must release its
resources. Opening the database after a crash must run recovery correctly.

With embedding, the database shares the application's process. A crash in
native code can take down the whole application. This makes testing session
separation, cleanup, and recovery especially important.

The current native (non-Wasm) build has passed the project's full test
suite on CentOS 7.2, Ubuntu 16.04, Rocky Linux 9.8, and Ubuntu 24.04.
Those runs include C and Python package checks.

There is also a
[weekly upstream compatibility workflow](https://github.com/postgamma/postgamma/blob/cc19919ef8aa1c6ccf4ddb87e9a555f6cac884cb/.github/workflows/upstream-canary.yml).
It tests the transformation against new commits on PostgreSQL's
`REL_19_STABLE` and `master` branches.

## Where it stands

The current native target is PostgreSQL 19 on Linux x86-64. The code is
available under the Apache 2.0 license. Version 0.1.0a1 is now available as
Python wheels and a static C SDK. See the
[download page](https://postgamma.com/downloads/) for both.

There are limits. Only one process can use a given data directory at a time.
The embedded version supports bundled extensions such as pgvector. It does
not load arbitrary PostgreSQL extension libraries. The
[compatibility page](https://postgamma.com/compatibility/limits/)
has more details.

Wasm support is still in development.

I'd like feedback from people who build local tools or work on databases.
I'm especially interested in feedback on the source transformation
approach. Workloads that expose problems with threads, cancellation, or
connection reuse would also help.

The code is on [GitHub](https://github.com/postgamma/postgamma). The
[documentation](https://postgamma.com/getting-started/) has examples and
build instructions.
