# Static C quickstart

This tutorial links PostGamma into a native executable, creates a PostgreSQL
cluster directory, runs a parameterized query, checkpoints the cluster, and
shuts it down. The resulting executable has no runtime dependency on
`libpostgamma.so`.

The static product has two parts:

| Part | Used when | Purpose |
| --- | --- | --- |
| `libpostgamma.a`, headers, and `postgamma.pc` | Compile and link | Host API and embedded kernel |
| Matching PostgreSQL resource pack | Application runtime | Catalog bootstrap data, time zones, SQL files, and executable identity |

The archive is not a self-contained database without its matching resource
pack.

## 1. Unpack the static SDK

Download these two assets from the same PostGamma release:

- `postgamma-sdk-{{ POSTGAMMA_VERSION }}-linux-x86_64.tar.gz`
- `postgamma-sdk-{{ POSTGAMMA_VERSION }}-linux-x86_64.tar.gz.sha256`

Verify and unpack the archive:

```bash
sha256sum --check postgamma-sdk-{{ POSTGAMMA_VERSION }}-linux-x86_64.tar.gz.sha256
tar -xzf postgamma-sdk-{{ POSTGAMMA_VERSION }}-linux-x86_64.tar.gz
```

Set paths for the remaining commands. The SDK and runtime resources are
children of the same release directory:

```bash
export POSTGAMMA_SDK="$PWD/postgamma-sdk-{{ POSTGAMMA_VERSION }}-linux-x86_64"
export POSTGAMMA_RESOURCES="$POSTGAMMA_SDK/resource-pack"
export PKG_CONFIG_PATH="$POSTGAMMA_SDK/lib/pkgconfig"
```

Verify both halves before compiling:

```bash
test -f "$POSTGAMMA_SDK/lib/libpostgamma.a"
test -f "$POSTGAMMA_SDK/include/postgamma/postgamma.h"
test -f "$POSTGAMMA_RESOURCES/share/postgres.bki"
test -f "$POSTGAMMA_RESOURCES/bin/postgres"
pkg-config --modversion postgamma
```

The current C SDK version is printed by the final command. Do not combine a
library and resource pack from different release builds.

## 2. Compile the validated example

The release contains a complete program at `examples/quickstart.c`. It checks
ABI identity, opens an instance, opens a connection, creates a table, binds
typed parameters, reads a result, runs a checkpoint, and releases every owned
handle.

Compile it with the generated static link contract:

```bash
cc -std=c11 -O2 -Wall -Wextra -Wpedantic \
  "$POSTGAMMA_SDK/examples/quickstart.c" \
  $(pkg-config --cflags --static --libs postgamma) \
  -o postgamma-quickstart
```

Always use `pkg-config --static`. A command containing only
`-lpostgamma` omits the platform libraries selected by the build profile.

## 3. Run it

Create the parent directory, then pass the cluster path and resource paths:

```bash
mkdir -p "$PWD/data"
./postgamma-quickstart \
  "$PWD/data/c-agent.pgm" \
  "$POSTGAMMA_RESOURCES/bin/postgres" \
  "$POSTGAMMA_RESOURCES" \
  create
```

The final application line is similar to:

```text
POSTGAMMA_QUICKSTART abi=65539 capabilities=112639 postgres=19 rows=1 name=planner score=98.5 checkpoint=true phase=closed
```

The first argument is the persistent cluster directory. The second is the
PostgreSQL executable identity shipped in the resource pack; PostGamma uses it
for PostgreSQL path semantics but does not launch it. The third is the resource
root. The final `create` argument tells this example to permit cluster
initialization.

System tracing in the SDK validation rejects process creation, network
endpoints, delivery of real signals to the host, and process-global working
directory changes on this path.

## 4. Recognize the C lifecycle

Every normal C application follows the same ownership order:

```text
PGM_INSTANCE_OPTIONS_INIT
        |
        v
pgm_instance_open
        |
        v
PGM_CONNECTION_OPTIONS_INIT -> pgm_connection_open
        |
        v
pgm_execute -> inspect pgm_result -> pgm_result_free
        |
        v
pgm_connection_close
        |
        v
pgm_instance_close
```

The essential setup in the example is:

```c
pgm_instance_options instance_options = PGM_INSTANCE_OPTIONS_INIT;
pgm_connection_options connection_options = PGM_CONNECTION_OPTIONS_INIT;
pgm_instance *instance = NULL;
pgm_connection *connection = NULL;
pgm_error *error = NULL;

instance_options.path = cluster_path;
instance_options.create = 1;
instance_options.executable_path = postgres_identity;
instance_options.resource_root = resource_root;

pgm_status status = pgm_instance_open(
    &instance_options, &instance, &error);
if (status != PGM_STATUS_OK)
    /* Report error, free it, and stop. */

connection_options.user = "postgamma";
connection_options.database = "postgres";
status = pgm_connection_open(
    instance, &connection_options, &connection, &error);
```

Use the complete repository example for error handling and cleanup. In
particular:

- initialize every public structure with its `PGM_*_INIT` macro;
- free every `pgm_result` with `pgm_result_free()`;
- free every returned `pgm_error` with `pgm_error_free()`;
- close child handles before their parent connection or instance;
- treat borrowed result values as valid only while their owning result lives;
- retry a failed close only when its function contract permits it.

## 5. Package the application

`libpostgamma.a` is consumed at link time. A deployed application needs:

- the linked host executable;
- the exact matching resource pack as a complete directory;
- any dynamic platform libraries reported by the final executable;
- the PostGamma, PostgreSQL, and pgvector license notices supplied with the
  release.

It does not need `libpostgamma.a`, public headers, or `postgamma.pc` at runtime.
It also does not need a running PostgreSQL service.

Static PostGamma linkage is not the same as a fully static Linux executable.
glibc and optional build dependencies such as zlib or ICU may remain dynamic.
Build against the oldest supported sysroot; see
[Platforms and glibc](../compatibility/platforms.md).

## Build the release archive from source

Maintainers and source consumers can reproduce the same layout:

```bash
git submodule update --init --recursive
make static-sdk-package JOBS=8
```

The command first runs the installed-consumer and host-safety gates, then
writes the archive and its machine-readable receipt below `build/dist/`. It
fails closed unless the PostGamma, PostgreSQL, and pgvector license files are
present.

## Next steps

- [Build, link, and package the SDK](../c/build-and-link.md)
- [Understand C ownership and concurrency](../c/ownership-and-concurrency.md)
- [Run asynchronous requests](../c/async.md)
- [Look up the complete C API](../c/reference.md)
- [Troubleshoot startup and linking](../troubleshooting.md)
