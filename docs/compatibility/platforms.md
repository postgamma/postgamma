# Platforms and glibc

## Release baseline

The first Python release targets Linux x86-64, CPython 3.10 through 3.14, and
the `manylinux_2_17_x86_64` policy. A compliant wheel must run on mainstream
Linux distributions with glibc 2.17 or newer.

The C SDK must be built and consumer-linked against the same minimum sysroot
before it receives the release compatibility claim.

The release workflow pins a `manylinux2014_x86_64` container by image digest.
GCC 10 compiles all shipped C objects with `-march=x86-64 -mtune=generic`.
LLVM/Clang 22 and its full LibTooling closure are separately checksum-locked
and build only the source-transformation tools; those libraries are not part
of either released runtime. The tested static kernel is then reused for every
CPython wheel in the matrix.

The manylinux2014 image is CentOS 7 based, but the published contract is the
glibc 2.17 ABI floor rather than support for that distribution as an operating
system. The pinned container supplies an old sysroot while current target
distributions supply their own glibc.

## Why the build host matters

A final shared object or executable records the versions of glibc symbols it
uses. Building on a recent developer distribution can bind newer symbols and
make the result fail to load on an older system.

A static PostGamma archive is relocatable and does not itself contain an ELF
`DT_NEEDED` entry for libc. It can nevertheless contain references to APIs that
an older libc does not provide. The final consumer link can therefore fail even
when PostGamma itself is supplied as `.a`.

For this reason:

- release artifacts are configured and compiled inside the minimum manylinux
  environment;
- product objects use the generic x86-64 instruction baseline;
- the final wheel is repaired and inspected with `auditwheel`;
- the highest referenced `GLIBC_*` version must not exceed the policy floor;
- a real executable links against the static SDK inside the minimum container;
- both products execute tests in the minimum environment, not only on the
  developer host.

## libc is host-owned

PostGamma does not bundle glibc into `libpostgamma.a`. Fully static glibc is not
the default because name-service, locale, dynamic loading, and other host
integration behavior depend on the target system.

Dependencies such as zlib or ICU follow a separate packaging decision: they may
be statically incorporated when compatible or bundled as ordinary wheel shared
libraries. They do not change the rule that glibc remains host-owned.

## Developer builds

Artifacts under `build/` prove code behavior on the current host. They do not
prove the manylinux floor. In particular, a wheel built on Ubuntu 24.04 must not
be relabeled as `manylinux_2_17` without the release repair and compatibility
gates.

## Other platforms

musllinux, macOS, Windows, additional CPU architectures, and stable-ABI Python
wheels are not part of the current product contract. They require independent
build, dependency, stack-size, lifecycle, and conformance evidence.
