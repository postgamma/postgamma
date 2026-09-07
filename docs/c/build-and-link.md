# Build, link, and deploy

The first C release is a static SDK. Link `libpostgamma.a` into the host
executable and deploy the matching PostgreSQL resource pack as a directory.

## SDK layout

An SDK prefix has this public layout:

```text
include/postgamma/
├── postgamma.h
├── postgamma_arrow.h
└── postgamma_extension.h
lib/
├── libpostgamma.a
├── postgamma-static-libs.txt
└── pkgconfig/postgamma.pc
```

The resource pack is a child of the same release archive. At minimum,
applications should validate these entries before opening a cluster:

```text
resource-pack/
├── bin/postgres
└── share/postgres.bki
```

Do not copy individual resource files. Preserve the complete tree supplied by
the release.

## Link with `pkg-config`

Point `PKG_CONFIG_PATH` at the selected SDK prefix:

```bash
export POSTGAMMA_SDK="$PWD/postgamma-sdk-{{ POSTGAMMA_VERSION }}-linux-x86_64"
export POSTGAMMA_RESOURCES="$POSTGAMMA_SDK/resource-pack"
export PKG_CONFIG_PATH="$POSTGAMMA_SDK/lib/pkgconfig"
```

Inspect the exact profile before linking:

```bash
pkg-config --modversion postgamma
pkg-config --cflags --static --libs postgamma
```

Compile a C11 application:

```bash
cc -std=c11 -O2 app.c \
  $(pkg-config --cflags --static --libs postgamma) \
  -o app
```

`--static` asks `pkg-config` to include `Libs.private`, such as zlib, math,
pthread, and ICU libraries selected by that PostGamma build. It does not ask
the compiler to create a fully static Linux executable.

Do not reconstruct PostgreSQL's internal archive list. The generated
`postgamma.pc` and `postgamma-static-libs.txt` files are the consumer link
contract for their exact build.

## Configure runtime paths

The C API does not guess an installation prefix. Supply the cluster and
resource paths explicitly:

```c
pgm_instance_options options = PGM_INSTANCE_OPTIONS_INIT;

options.path = "/var/lib/my-agent/agent.pgm";
options.create = 1;
options.resource_root = "/opt/postgamma/resource-pack";
options.executable_path = "/opt/postgamma/resource-pack/bin/postgres";
```

`path` is the writable PostgreSQL cluster directory. `resource_root` is the
read-only runtime resource tree. `executable_path` is the PostgreSQL executable
identity from that tree. The embedded lifecycle does not execute it.

Use absolute paths in services. PostGamma does not change the host working
directory, and relative application paths remain subject to changes made by
the host itself.

## What the final executable depends on

After linking, inspect the consumer rather than assuming the dependency set:

```bash
readelf -d ./app
ldd ./app
```

The executable must not contain a `NEEDED` entry for `libpostgamma.so` and must
not contain a build-workspace `RPATH` or `RUNPATH`. It may still depend on
system libraries selected by the build profile.

Static linking localizes the private PostgreSQL kernel inside the final link.
It does not bundle glibc. A static archive built on a recent distribution can
reference libc functions unavailable on an older one, so release archives must
be built and consumer-linked against the oldest supported sysroot.

The initial release baseline is glibc 2.28. A developer archive built on a
newer system carries no automatic compatibility guarantee. See
[Platforms and glibc](../compatibility/platforms.md).

## Files to deploy

For a normal native application, deploy:

| File or directory | Runtime requirement |
| --- | --- |
| Host executable containing PostGamma | Required |
| Complete matching resource pack | Required |
| Dynamic platform libraries reported by the final ELF | Required unless supplied by the target OS |
| PostGamma, PostgreSQL, and pgvector notices | Required by the release license package |
| `libpostgamma.a` | Not required after final link |
| Public headers and `postgamma.pc` | Not required after final link |

Keep the resource directory read-only to the application where practical. The
cluster directory must be writable and should have private filesystem
permissions.

## Validate a release build

From the source tree:

```bash
make static-sdk-package JOBS=8
```

The gate performs all of the following against a staged installed layout:

1. verifies the deterministic static archive and its public symbol set;
2. validates relocatable headers and `pkg-config` metadata;
3. compiles a standalone consumer with warnings treated as errors;
4. rejects a PostGamma shared-library dependency and local runtime paths;
5. runs a create/query/checkpoint/close lifecycle;
6. traces the consumer and rejects process, network, host-signal, and
   process-global-state violations.

Passing on a developer workstation proves that build and creates the release
archive below `build/dist/`. The official binary release must repeat the
consumer link and execution inside the minimum supported environment.

## Common link failures

| Failure | Cause | Fix |
| --- | --- | --- |
| Undefined zlib, ICU, math, or pthread symbols | Linked without `pkg-config --static` | Use the generated static link flags |
| `postgamma.pc` not found | Wrong `PKG_CONFIG_PATH` | Point it at the SDK's `lib/pkgconfig` directory |
| Runtime resource error | Missing or mismatched resource pack | Deploy the complete pack from the same build |
| Loads on build host but not target | Newer libc symbols were selected | Rebuild against the release sysroot |
| Unexpected `libpostgamma.so` dependency | Shared link flags or library precedence | Inspect the link command and staged SDK |

For startup and data-directory failures, continue with
[Troubleshooting](../troubleshooting.md).
