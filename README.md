# PostGamma

[postgamma.com](https://postgamma.com)

PostGamma embeds PostgreSQL 19 directly inside an application process. Open a
local cluster directory, execute PostgreSQL SQL, and close it without starting
a database server or opening a network port.

The first release provides:

- a CPython package with synchronous, DB-API 2.0, and caller-driven asyncio
  interfaces;
- a symbol-isolated static C SDK with a matching runtime resource pack;
- PostgreSQL transactions, WAL, crash recovery, COPY, prepared statements,
  notices, notifications, and logical dump and restore;
- multiple concurrent logical connections and multiple isolated instances in
  one host process; and
- bundled pgvector 0.8.6 for local vector search.

This project is an unreleased alpha. Read the
[compatibility limits](docs/compatibility/limits.md) before using it with data
that cannot be recreated.

## Python quickstart

Install a compatible release wheel:

```bash
python -m pip install postgamma
```

Then open or create a persistent cluster and run SQL:

```python
import postgamma

with postgamma.connect("agent.pgm", autocommit=True) as connection:
    connection.execute(
        "create table if not exists notes("
        "id bigint primary key, body text not null)"
    )
    connection.execute(
        "insert into notes values ($1, $2) "
        "on conflict (id) do update set body = excluded.body",
        [1, "local"],
    )
    print(connection.execute("select * from notes").fetchall())
```

```text
[(1, 'local')]
```

`agent.pgm` is a PostgreSQL cluster directory, not a DSN and not a single
SQLite-style file. If the final directory is missing, `connect()` creates it.
Its parent directory must already exist. If it contains a compatible cluster,
PostGamma opens it without replacing its data.

The initial logical database is `postgres` and the initial role is
`postgamma`. Passing `database="application"` selects a database; it does not
create one. Use `OPEN_EXISTING` when a missing or misspelled production path
must fail:

```python
with postgamma.connect(
    "agent.pgm",
    mode=postgamma.OpenMode.OPEN_EXISTING,
) as connection:
    print(connection.execute("select current_database()").fetchone())
```

Continue with the [five-minute Python tutorial](docs/getting-started/index.md)
or the [complete Python guides](docs/python/index.md). See
[downloads](docs/downloads.md) for binary status and verification.

## Vector search

pgvector is linked into the embedded kernel. Enable its SQL objects once in
each logical database that needs them:

```python
with postgamma.connect("agent.pgm", autocommit=True) as connection:
    connection.execute("create extension if not exists vector")
    connection.execute(
        "create table if not exists items("
        "id bigint primary key, embedding vector(3))"
    )
```

No separate `vector.so` is loaded. See the
[pgvector guide](docs/extensions/pgvector.md) for HNSW, IVFFlat, and concurrent
query examples.

## Static C SDK

Native applications and language bindings can link `libpostgamma.a`. The SDK
archive contains:

- `include/postgamma/` with the public C and extension headers;
- `lib/libpostgamma.a` and relocatable `pkg-config` metadata;
- a matching PostgreSQL runtime resource pack;
- a complete C quickstart; and
- PostGamma, PostgreSQL, and pgvector license notices.

Compile consumers with the release's static link contract:

```bash
cc -std=c11 -O2 application.c \
  $(pkg-config --cflags --static --libs postgamma) \
  -o application
```

The linked executable has no runtime dependency on `libpostgamma.so`, but it
still needs the matching resource pack. Static PostGamma linkage does not imply
a fully static glibc executable. Follow the
[static C quickstart](docs/getting-started/c-static.md) for verification,
linking, deployment, and lifecycle details.

## Product boundary

PostGamma uses PostgreSQL's parser, planner, executor, catalogs, transactions,
WAL, and recovery code. Its application boundary deliberately differs from a
server deployment:

- PostgreSQL roles run as native threads in the host process;
- logical connections communicate through bounded in-memory transport;
- no SQL listener, TCP endpoint, or server subprocess is created;
- no host signal handler, working directory, or process timer is silently
  replaced;
- one cluster directory has one live operating-system process owner;
- arbitrary native extensions cannot be loaded at runtime; supported extensions
  must be reviewed and linked into the product; and
- cluster directories are tied to PostgreSQL major version 19.

The public API does not expose pipeline mode, COPY BOTH and replication protocol,
raw PostgreSQL protocol access, or C transaction callbacks.

Multiple live instances are supported in one host process when each uses a
separate cluster directory.

The Python binding has its own API and packaging validation; passing the C ABI
compatibility gate alone does not validate the Python package.

Use SQLite when a tiny single-file database is the primary requirement, DuckDB
for analytics-first columnar execution, PGlite for JavaScript/WASM, or a normal
PostgreSQL server when independent processes must share one live cluster.

Read [When to use PostGamma](docs/getting-started/why-postgamma.md) for the
full comparison.

## Build from source

PostGamma's supported source-build host is Linux on x86-64 with glibc 2.28 or
newer. After cloning the repository, initialize the pinned PostgreSQL and
pgvector submodules:

```bash
make source-init
```

Both `postgres/` and `third_party/pgvector/` are Git submodules pinned to exact
commits. The main repository records commit references; `make source-init`
populates both directories with their source files. PostgreSQL defaults to its
official Git server. To use the [PostgreSQL GitHub mirror](https://github.com/postgres/postgres):

```bash
make source-init PG_REPOSITORY=https://github.com/postgres/postgres.git
```

The URL is saved in this checkout's Git configuration and also applies to later
upstream fetches. It does not change the pinned commits or `.gitmodules`.
`PGVECTOR_REPOSITORY` provides the same override for pgvector. Omit the variables
to retain the current URLs, or pass the original URL to switch back.

Choose the artifact you need before installing dependencies. Each workflow has
a focused environment check and a matching build target:

| Goal | Check the environment | Build target | Primary output |
| --- | --- | --- | --- |
| Threaded PostgreSQL core | `make doctor-threaded JOBS=4` | `make threaded-postgresql-build JOBS=4` | `build/generated/postgres/` and `build/generated-build/` |
| Static C SDK | `make doctor-sdk JOBS=4` | `make static-sdk-package JOBS=4` | `build/dist/postgamma-sdk-*.tar.gz` |
| Python wheel | `make doctor-python JOBS=4` | `make python-package-check JOBS=4` | `build/python-product/wheel/*.whl` |
| Documentation site | `make doctor-docs` | `make docs` | `build/docs/site/` |
| Complete local release | `make doctor JOBS=4` | `make release-candidate JOBS=4` | All of the above plus `build/dist/postgamma-*-local-release.json` |

The build targets repeat their corresponding environment checks, so the first
command in each row is optional. Running it separately is recommended on a new
machine: it stops before the build, compiles small toolchain probes, and prints
platform-specific installation commands for missing dependencies.

`make doctor` is deliberately the complete check. It covers the native SDK,
the full PostgreSQL test toolchain, Python wheel packaging, and documentation.
Use `doctor-threaded`, `doctor-sdk`, `doctor-python`, or `doctor-docs` when you
only need that workflow and do not want unrelated tools to block the build.
The exact package requirements and supported Linux families are listed in the
[installation guide](docs/getting-started/installation.md).

### Develop the threaded PostgreSQL core

Use this path when working on the PostgreSQL source transformation itself:

```bash
make doctor-threaded JOBS=4
make threaded-postgresql-build JOBS=4
```

The build leaves the transformed source in `build/generated/postgres/` and the
out-of-tree PostgreSQL build in `build/generated-build/`. Both directories are
reproducible build products; change the adapters, manifests, transformer, or
runtime sources instead of editing either directory directly.

The build target compiles the threaded tree but does not run the complete test
suite. Before submitting threading changes, validate both pristine and
threaded PostgreSQL, the transformation, sanitizer runtime, and thread
isolation contracts:

```bash
make threaded-postgresql-check JOBS=4
```

### Follow upstream PostgreSQL

Run the weekly compatibility suite against the latest PostgreSQL `master`:

```bash
make upstream-weekly
```

This initializes the source submodules if needed, fetches the selected branch,
and tests its resolved commit in an isolated worktree. It enables debug,
assertion, TAP, and injection-point tests; ICU is disabled by default. The
pinned PostgreSQL checkout remains at its recorded commit. Reports are written
to `build/reports/upstream-canary-<sha>-weekly.json`.

The weekly suite includes pristine and threaded `check-world`, runtime
sanitizers, and one hour of thread-isolation stress. Use `make upstream-daily`
for the shorter build and runtime checks. Both commands accept `PG_BRANCH`
(default `master`), `PG_REPOSITORY`, and `JOBS`; for example:

```bash
make upstream-weekly PG_BRANCH=REL_19_STABLE JOBS=4
```

The lower-level `upstream-canary` target accepts `PG_REF=<commit>` to replay a
fetched candidate with explicit build settings. A successful canary reports
compatibility evidence; updating the product's pinned version remains a
separate reviewed change.

GitHub Actions checks for updates every Sunday at 02:17 UTC on the default
branch. It compares the PostGamma commit and the resolved PostgreSQL commit
against previous test results, separately for `master`, `REL_19_STABLE`, and
the pinned embedded product. An unchanged combination with a passing result
skips dependency installation, builds, and tests. Changed inputs, a failed
result, or missing history cause the corresponding tests to run. Each upstream
test uses the exact PostgreSQL commit checked by this comparison.

To force a run, choose **PostgreSQL upstream canary** in GitHub Actions,
select **Run workflow**, and choose the `weekly` or shorter `daily` profile.
Manual runs and job retries always run the tests, as do the local Make targets.
Repository Actions variables `PG_REPOSITORY` and `PGVECTOR_REPOSITORY` select
download mirrors for CI too; changing either also causes tests to run.

### Build the static C SDK

Use this path for native applications and for new language bindings:

```bash
make doctor-sdk JOBS=4
make static-sdk-package JOBS=4
```

The target builds and tests the embedded kernel, then writes a versioned SDK
archive, SHA-256 checksum, and release receipt under `build/dist/`. Start with
the archive's included example or continue with the
[static C quickstart](docs/getting-started/c-static.md).

### Build the Python wheel

Use the same Python interpreter for the environment check and the wheel you
intend to test:

```bash
python3 -m venv .venv
. .venv/bin/activate
make doctor-python JOBS=4
make python-package-check JOBS=4
```

The validated wheel is written to `build/python-product/wheel/`. This is a
developer-native wheel for the current interpreter and host; official wheel
publication additionally uses the repository's external manylinux and
multi-interpreter release matrix.

### Build the documentation

```bash
make doctor-docs
make docs
```

The documentation build treats warnings and broken references as errors. The
static site is written to `build/docs/site/`; see
[Documentation website](#documentation-website) to preview it locally.

### Build a complete local release candidate

Maintainers can run the complete local release gate with:

```bash
make doctor JOBS=4
make release-candidate JOBS=4
```

This validates pristine and threaded PostgreSQL, exercises the embedded
runtime, compares a versioned SQL corpus with PostgreSQL 19, runs the Python
integration suite, packages the static SDK and local wheel, builds the
documentation in strict mode, and writes a hash-bound receipt to `build/dist/`.

It does not upload anything or claim that the developer-native wheel has
passed the external manylinux interpreter matrix. Long-running stress and soak
targets are also separate from this local gate.

### Choose the parallelism

`JOBS` controls build parallelism, not which checks run. `JOBS=4` is a
conservative starting point for a machine with about 8 GiB of memory. Increase
it when CPU and memory allow, or reduce it if the compiler is killed for lack
of memory. Every doctor command reports the detected CPU count, available
memory, free disk space, and the effective `JOBS` value.

### Run a focused validation target

The primary artifact commands above are the normal entry points. Use these
targets when diagnosing or validating a specific subsystem:

| Command | Purpose |
| --- | --- |
| `make embedded-runtime-fast-check JOBS=4` | Run the fast in-process lifecycle and query gates |
| `make embedded-runtime-check JOBS=4` | Run the complete embedded product gate |
| `make embedded-conformance-check JOBS=4` | Compare the public API with PostgreSQL 19 |
| `make embedded-soak-check JOBS=4` | Run 1,000 in-process lifecycle iterations |
| `make version-check` | Validate the public version, tag, and release mirrors |

Run `make help` for the public command list. Maintainers of existing build
automation can use `make help-internal` for lower-level compatibility targets.
If a check fails, install the packages named by the doctor and rerun the same
command; intermediate outputs and diagnostic evidence remain under `build/`.
Public releases follow the checked
[versioning policy](docs/releases/versioning.md).

## Documentation website

Build the complete site:

```bash
make docs
```

Serve it locally on loopback:

```bash
make docs-serve
```

To access it from another machine, bind the development server to all network
interfaces and open the selected port in the host firewall:

```bash
make docs-serve DOCS_DEV_ADDR=0.0.0.0:8000
```

Then visit `http://SERVER_IP:8000/`. The development server is for trusted
networks only; publish `build/docs/site/` behind a normal static web server for
public access.

## Supported release target

The initial binary contract is:

| Component | Supported target |
| --- | --- |
| Operating system | Linux |
| Architecture | x86-64 |
| libc | glibc 2.28 or newer for official artifacts |
| Python | CPython 3.10 through 3.14 |
| PostgreSQL cluster format | Major version 19 |
| C boundary | Versioned PostGamma ABI |

Developer artifacts built on a newer distribution may require a newer glibc.
Only artifacts built against the minimum release sysroot carry the published
glibc floor.

## Contributing and security

See [CONTRIBUTING.md](CONTRIBUTING.md) for source rules and pull requests. Send
security-sensitive reports through the process in [SECURITY.md](SECURITY.md),
not through a public issue.

Copyright 2026 Shujie Zhang. PostGamma-owned source and documentation are
licensed under [Apache License 2.0](LICENSE). PostgreSQL and pgvector retain
their PostgreSQL License terms. See [NOTICE](NOTICE) and
[THIRD_PARTY_NOTICES](THIRD_PARTY_NOTICES) for attribution and artifact details.
