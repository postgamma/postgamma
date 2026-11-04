---
title: PostgreSQL in Your Browser with PostGamma
description: A real PostgreSQL engine and psql terminal, running locally in your browser with WebAssembly.
author: Shujie Zhang
categories:
  - Announcements
draft: false
date: 2026-11-03
slug: postgresql-in-your-browser-with-postgamma
cover: assets/images/blog/postgresql-in-your-browser.webp
cover_alt: A tactile paper-cut blue elephant entering a browser-shaped opening in a mint and coral landscape.
---

# PostgreSQL in Your Browser with PostGamma

Open the [PostGamma homepage](https://postgamma.com/#live-demo), wait for the
`postgres=#` prompt, and type:

```sql
SELECT version();
```

The query runs on your device. The page loads a PostgreSQL engine compiled to
WebAssembly, together with the real upstream psql client. There is no remote
SQL server behind the terminal.

This is the browser preview of PostGamma. You can try transactions, JSONB,
pgvector, and PostgreSQL's query planner without installing a database server.

## From an application library to a browser tab

PostGamma started with a goal: let an application open PostgreSQL as a library.
The native version embeds the engine in C and Python applications. It runs
PostgreSQL's work in threads, with separate state for each session.

The browser build takes that work into WebAssembly. The engine runs in browser
Workers, and the page communicates with it through a JavaScript API. PostgreSQL
still parses the SQL, plans the query, executes it, and manages the database.

The Live terminal is one way to use that engine. An application can also use the
SDK directly, without displaying a terminal. Both interfaces run locally in the
browser.

## Start with a small database

Here is a complete example to paste into Live:

```sql
CREATE TEMP TABLE notes (
    id integer PRIMARY KEY,
    document jsonb NOT NULL
);

INSERT INTO notes VALUES
    (1, '{"title":"Browser SQL","tags":["postgres","local"]}'),
    (2, '{"title":"A small experiment","tags":["scratchpad"]}');

SELECT id, document->>'title' AS title
FROM notes
WHERE document @> '{"tags":["local"]}';
```

The result is one row: `Browser SQL`.

You can keep using psql commands you already know. Type `\d notes` to inspect
the table, `\dt` to list tables, or `\?` for help. Unfinished SQL gets a
continuation prompt. The arrow keys edit input and recall earlier commands.
Ctrl+C cancels a query or clears unfinished input.

Transactions work too. Continue in the same session:

```sql
BEGIN;
DELETE FROM notes;
SELECT count(*) FROM notes;
ROLLBACK;
SELECT count(*) FROM notes;
```

The first count is zero. After the rollback, both rows are there again.

These examples use a temporary table. The homepage also starts a fresh
in-memory database: reloading the page or resetting the instance discards its
data.

## Vector search, in the same engine

The preview includes pgvector. You can create a vector column and query it
alongside ordinary PostgreSQL data:

```sql
CREATE EXTENSION IF NOT EXISTS vector;

CREATE TEMP TABLE topics (
    title text,
    embedding vector(3)
);

INSERT INTO topics VALUES
    ('PostgreSQL in the browser', '[1,0,0]'),
    ('Local vector search', '[0.8,0.2,0]'),
    ('Native applications', '[0,0,1]');

SELECT title
FROM topics
ORDER BY embedding <-> '[1,0,0]'
LIMIT 2;
```

The first result is `PostgreSQL in the browser`, followed by `Local vector
search`. These are hand-written vectors so the example is self-contained.
An application using text embeddings would supply those vectors separately.

Expand the terminal to open **Examples**. **Run example** executes a script in
the same session and checks its result. There are examples for aggregation,
JSONB, transactions, vector search, and a parallel query. The parallel example
checks that PostgreSQL actually launches two workers; the terminal shows the
`EXPLAIN ANALYZE` output.

## What could you build with it?

One starting point is an interactive SQL tutorial. Each reader gets a local
database to explore, change, and reset. Exercises can use PostgreSQL syntax and
extensions without a shared demonstration server.

Another is a browser tool for exploring data. An application can import a
sample, let the user query it, and combine relational filters with JSONB or
vector operations. The database engine does not need a remote service to run
those queries. Whether the rest of the application sends data elsewhere is
still an application design choice.

For applications that need to keep their database after a reload, the SDK also
supports the browser's Origin Private File System, or OPFS. The separate
[OPFS persistence check](https://postgamma.com/opfs/) demonstrates saving a record and
reopening the database. Storage belongs to the site's origin; clearing site
data removes it. The homepage terminal deliberately uses memory storage.

## A preview with concrete boundaries

This build targets PostgreSQL 19. It is an experimental browser package built
from source; an npm registry package has not been published yet. The
[browser guide](../../getting-started/browser.md) explains how to build and
host it, including the required assets and browser isolation headers.

The package has been exercised in Chromium, Firefox, and Linux WebKit. That
does not establish support for physical Safari, iPhones, or other mobile
devices. The browser runtime also has a substantial memory footprint; it is
worth measuring startup and memory use on the devices your application targets.

The terminal supports SQL and local database connections. It cannot run shell
commands or connect to a remote PostgreSQL server. Extensions are bundled into
the build rather than loaded as arbitrary native libraries. See the
[preview notes](../../releases/wasm-0.1.0-alpha.1.md) for the current limits and
qualification status.

## Try it and tell me what you build

[Open Live](https://postgamma.com/#live-demo) and run a query. If you prefer to start
from code, follow the [browser SDK guide](../../getting-started/browser.md).
The source is on [GitHub](https://github.com/postgamma/postgamma).

I'd like to hear from people building browser tools, interactive documentation,
or local data applications. What would you use a PostgreSQL database inside a
browser for? What would stop you from using this preview?

The next article explains [how the thread-based runtime becomes a WebAssembly
build](05-how-postgamma-runs-postgresql-in-webassembly.md).
