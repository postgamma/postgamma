---
title: A Repeated Check in PostGamma
description: Why threads can help with many short queries, and how I removed a repeated check from state access.
author: Shujie Zhang
categories:
  - Engineering
draft: false
date: 2026-10-31T13:19:02+00:00
slug: a-repeated-check-in-postgamma
cover: assets/images/blog/a-repeated-check-in-postgamma.webp
cover_alt: An illustrated debugging desk with an abstract orange flame graph and a zoomed view of a repeated branch.
---

# A Repeated Check in PostGamma

One useful case for threads is a busy server handling many short queries.
The SQL itself may be cheap, but switching between sessions still costs time.

During a performance investigation, I found extra CPU work in PostGamma's
state access code. The database could run the queries correctly, but the
runtime was doing some of the same work again and again.

This post follows one small part of that problem: checking the same table
pointer every time PostgreSQL reads a state variable. At the end, I also
show a separate test with many active sessions.

The state-access experiment is from October 10, 2026. It compares two
versions of the threaded server. Both experiments in this post use the
server, rather than the embedded C or Python library.

## Why a variable read needs runtime code

PostGamma runs PostgreSQL with threads. State that used to belong to a
backend process now has an explicit owner. That owner may be an instance,
a PostgreSQL execution role, or a session. Here, a role means a runtime task
such as a backend or checkpointer, not a SQL user role.

Generated code needs a way to find the right value. In this version, it
uses a table of state addresses. Each variable has a slot in the table.

By this point, I had already added address caching. The runtime resolves
and initializes a slot on its first use. Later accesses can use the saved
address.

But even a cached access still had several steps. It loaded the current
table pointer, checked that pointer, read the slot, and checked whether
the slot was ready.

The repeated work was in ordinary paths, including transaction setup and
memory management. A small cost there can occur many times during a query.

## The check I focused on

This is a shortened example of the old access path:

```c
table = current_state_addresses;

if (table == NULL)
    fail();

address = table[slot];
if (address == NULL)
    address = resolve_and_initialize(slot);
```

The real code also checks the slot number and its initialization template.
I left those checks out here to keep the example small.

The table-pointer check had a useful job: rejecting access to state
without an active execution context. I needed to keep that error visible.

But during normal execution, the same table pointer was checked over and
over. The runtime already controlled when that pointer could change. It
changed when a worker entered a context or restored the previous one.

That gave me a way to avoid repeating this check on every access.

## Always give the worker a readable table

I changed the runtime so the table pointer always points to a readable
table.

When a worker has an active context, it points to that context's table.
When there is no active context, it points to a shared empty table. Every
entry in the empty table is `NULL`, and the runtime never changes it.

The normal access path can then do this:

```c
table = current_state_addresses;
address = table[slot];

if (address == NULL)
    address = resolve_and_initialize(slot);
```

The runtime checks the context and table when binding or restoring them.
The slot still has its own first-access check.

An access without an active context is still an error. It reads a `NULL`
entry from the empty table and enters the existing resolver. The resolver
rejects the missing context before it can initialize anything.

The address cache belongs to the logical execution context. A worker
holds a pointer to that cache while running the context. This matters
when a session later runs on a different worker.

You can see the
[state access code](https://github.com/postgamma/postgamma/blob/cc19919ef8aa1c6ccf4ddb87e9a555f6cac884cb/runtime/include/postgamma/backend_state_runtime.h)
and the
[binding and restore code](https://github.com/postgamma/postgamma/blob/cc19919ef8aa1c6ccf4ddb87e9a555f6cac884cb/runtime/src/execution_context.c)
in the repository.

## Checking that the change was safe

I checked warm accesses, first accesses, and accesses without a context.
Warm accesses must keep using the cache. An invalid access must still
fail.

The tests also covered separate role and session state, nested contexts,
and moving an execution context between worker threads. After a nested
call, the caller must get its original table back.

The tested version passed PostgreSQL regression and isolation tests.
Runtime tests also passed with memory and thread sanitizers.

There was a separate pointer initialization fix in that version too.
It fixed how some generated state containing pointers was initialized. The
timings below compare the complete builds, so they do not isolate the
effect of one removed check.

## Measuring the state-access change

I used the same compiler and linker options, SQL, data, and CPU placement
for both versions. The client and server used separate CPUs. Each test
ran after warmup, with six alternating pairs of runs. Each run performed
100,000 operations and checked their results.

These were read-only tests on a memory-backed filesystem. The timing runs
did not use a profiler. The results below are median wall-clock time per
operation. Lower is better.

| Workload | Before | After | Time change |
| --- | ---: | ---: | ---: |
| `SELECT 1` over a connection | 61.808 us | 61.774 us | -0.05% |
| One prepared point query over a connection | 78.925 us | 80.018 us | +1.38% |
| A point-query loop inside PL/pgSQL | 10.087 us | 9.205 us | -8.74% |

The PL/pgSQL test runs many lookups inside one function call. Its time is
per lookup inside that loop. The network tests send a separate request
for each operation, so their absolute times measure different work.

The internal loop was faster in five of the six pairs. One pair was about
22% slower. The network point query was faster in only two pairs. I kept all
of the [timing samples](../evidence/state-access-check-2026-10-10.json).

`SELECT 1` stayed about the same. The network point query did not show a
stable improvement. The internal loop showed a lower median time, but
the results also show why I would not promise the same gain for every
workload.

This was an October development experiment. It measures a small change
between two threaded builds. It does not measure the current release
candidate against PostgreSQL.

## Why short queries make switching matter

The next test looks at a different question: how the threaded server
compares with PostgreSQL's normal process-based server under load.

Think of 16 open connections, each repeatedly asking for one row. With
four server CPUs, those CPUs keep moving between active backends.

A [normal PostgreSQL server](https://www.postgresql.org/docs/19/tutorial-arch.html)
uses a separate process for each connection. PostGamma's threaded server
runs its backends as threads in one process. They share an address space.

When a CPU switches directly between two of those threads, it can keep
the same address space. On Linux x86, the
[switching code](https://github.com/torvalds/linux/blob/v6.8/arch/x86/mm/tlb.c#L527-L550)
has a fast path for this case. That can reduce the cost of serving many
short queries as the CPU moves between sessions.

Threads still need scheduling, locks, and wakeups. The network server
still uses sockets. The benefit depends on how much time goes to
switching and networking compared with running the SQL.

## A separate test with many active sessions

On October 14, I compared a later threaded candidate with PostgreSQL's
normal process-based server. Both used the same PostgreSQL 19 beta source,
compiler policy, SQL, and data. This candidate included several runtime
changes beyond the one described above.

The clients kept their connections open and sent queries through ordinary
TCP and libpq. Each query read one row by its primary key. This measured
repeated work across active sessions, without opening a new connection
for every query.

The server had four assigned CPUs. The clients used two other CPUs. Each
case had three paired rounds, alternating which server ran first. A round
had three seconds of warmup and twelve seconds of measurement. The host
was a shared virtual machine.

Here is the prepared point query at different session counts. The numbers
are mean queries per second across the three rounds. Higher is better.

| Clients | PostgreSQL | PostGamma | Change |
| ---: | ---: | ---: | ---: |
| 1 | 9,883 | 9,375 | -5.1% |
| 4 | 37,693 | 36,889 | -2.1% |
| 16 | 39,982 | 50,033 | +25.1% |

At 16 sessions, the same point query using the simple protocol also had
higher throughput: **14.4% more queries per second**. Both point-query
tests were faster in all three paired rounds at that concurrency.

That is a useful case for the threaded design: many short queries, with
more active sessions than server CPUs. In the prepared point-query test at
16 sessions, PostGamma also used less measured server CPU time per query:
about 79 microseconds versus 99 for PostgreSQL.

This fits the reason to expect a benefit from threads. It does not isolate
the cost of switching address spaces from all the other runtime changes.
With 1 or 4 sessions, PostGamma's measured throughput was lower in this test.
It also used more CPU time per query at those session counts.
Other workloads differed too: at 16 sessions, a range aggregation was
9.1% slower in the threaded build.

The [full results and build identities](../evidence/short-query-concurrency-2026-10-14.json)
include all 21 workload and concurrency combinations, including slower
results. These are historical development results. They are separate from
the earlier state-access comparison and from current release performance.

I kept the simpler access path and its checks at context boundaries.
The useful part of this investigation was finding repeated work in
code that runs very often. The result also reminded me to check both
that inner path and the time a client actually sees.

For the state model behind this code, see
[Threading PostgreSQL Without Editing Upstream Source Code](02-threading-postgresql-without-editing-upstream-source-code.md).
