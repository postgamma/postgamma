---
title: Threading PostgreSQL Without Editing Upstream Source Code
description: PostGamma reads an unmodified PostgreSQL checkout and generates a separate threaded build.
author: Shujie Zhang
categories:
  - Engineering
draft: false
date: 2026-10-31T13:19:01+00:00
slug: threading-postgresql-without-editing-upstream-source-code
cover: assets/images/blog/threading-postgresql-without-editing-upstream.webp
cover_alt: An intact source blueprint beside layered transformations of a separate copy and three colorful execution tracks.
---

# Threading PostgreSQL Without Editing Upstream Source Code

PostGamma starts with an unmodified PostgreSQL checkout. It reads that
source and creates a separate tree with the changes needed for threads.

The upstream checkout stays untouched. PostGamma's rules and build tools
change only the generated copy. The runtime library supports the threaded code.

This approach produces a threaded PostgreSQL server. It also lets me build
an embedded library for C and Python applications.

## Why I stopped editing PostgreSQL directly

My first prototype used direct edits to PostgreSQL. With AI coding tools,
I had a version that compiled and passed regression tests after about
48 hours.

That was a useful start. But I would have to keep those edits working as
PostgreSQL changed. The prototype did not solve that maintenance problem.

So I changed the approach. I moved the work into a source transformation
and a runtime library. PostgreSQL's source became an input to the build.

## Keep the input and output separate

The build has a simple shape:

```text
Upstream PostgreSQL checkout
        (kept unchanged)
               |
               v
    Clang + PostGamma rules
               |
               v
      Generated source tree
               |
       + PostGamma runtime
               |
               v
Threaded server or embedded library
```

First, the build checks that the checkout is at the expected upstream
commit and has no local changes. The
[source-copy step](https://github.com/postgamma/postgamma/blob/cc19919ef8aa1c6ccf4ddb87e9a555f6cac884cb/buildsys/materialize.py)
then exports that commit into the build directory.

The transformation changes this copy. For example, accesses to backend
state can become calls into PostGamma's runtime.

For the threaded server build, that tree is in
`build/generated/postgres/`. I can open the generated C files, compare
them with upstream, and debug the result.

When I need to fix a transformation, I change its rules or build tools.
The next build generates the source again. I don't maintain edits inside
the generated directory.

This is what I mean by keeping the upstream source untouched. The output
is a modified PostgreSQL build, with PostGamma's runtime linked into it.

## Why use Clang

Clang parses the C code. It can find a variable's declaration and the
places that refer to it. This matters when different variables share the
same name.

PostGamma uses that information to prepare source replacements. The rules
say which state needs a new owner and which operations need runtime
support.

Clang does not decide the ownership model for me. I still have to write
and review those rules.

## One example is the session time zone

A PostgreSQL backend normally has its own process. Many writable global
variables are private to that process. Threads share an address space,
so those values need explicit owners.

Take `session_timezone`. One connection can use `UTC`. Another can use
`Asia/Shanghai`. Changing one connection's setting must leave the other
connection's setting alone.

The
[ownership rules](https://github.com/postgamma/postgamma/blob/cc19919ef8aa1c6ccf4ddb87e9a555f6cac884cb/manifests/ownership/backend-state-rules.json)
mark this variable as session state. The transformation redirects its
accesses to the active session's storage. The runtime binds a worker thread to
that state before it runs PostgreSQL code.

This lets me describe the ownership rule and generate the affected
accesses. I don't have to edit each use by hand.

Other values belong to a database instance or a PostgreSQL execution
role, such as a backend or checkpointer. Here, a role means a runtime task,
not a SQL user role. Some values can stay shared because they are read-only.
Those choices also live in the ownership rules.

## Check each generated change

The transformation produces a replacement plan: a list of exact text edits
for each file. Before applying it, the build checks each file's source hash.
It also checks the original text at each replacement location. A mismatch
stops the build.

These checks are in the
[replacement tool](https://github.com/postgamma/postgamma/blob/cc19919ef8aa1c6ccf4ddb87e9a555f6cac884cb/buildsys/apply_replacements.py).
They help catch a plan being applied to source it was not built for.
The tool also rejects replacements that overlap or fall outside a file.

The generated code is available for review. I can compare the original
source, the planned replacements, and the resulting C code when something
goes wrong.

## What the runtime still has to do

Generating the source is one part of making threads work. The runtime
still has to initialize state, route cancellation, manage waits, and
clean up a connection when it closes.

A new thread also needs explicit initialization rules. It does not get a
private copy of its parent's memory through `fork()`.

PostGamma keeps PostgreSQL's SQL parser, planner, executor, transactions,
WAL, and recovery code. The transformation changes how that code finds
state and works with the runtime.

Threads also share failures. In an embedded application, a native memory
fault can affect the host process. Keeping the upstream checkout clean
helps with maintenance; runtime behavior still needs careful testing.

## Following upstream changes

An upstream update can change assumptions in the rules. It may need new
rules or runtime work. I still need to generate the new tree and test it.

A
[weekly compatibility workflow](https://github.com/postgamma/postgamma/blob/cc19919ef8aa1c6ccf4ddb87e9a555f6cac884cb/.github/workflows/upstream-canary.yml)
tests PostgreSQL's `REL_19_STABLE` and `master` branches. The
current product target is PostgreSQL 19. Testing a development branch
helps find adaptation work early.

I also use PostgreSQL regression and isolation tests, runtime tests,
and C and Python package checks. They cover behavior that source checks
alone cannot prove.

This is the approach I want to keep: start from upstream PostgreSQL,
generate the changes, and test the result. The upstream code stays easy
to compare with the threaded build. The rules and runtime are the parts
I maintain in PostGamma.
