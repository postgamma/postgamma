# Releases and upgrades

Release notes are append-only. Read every version between the build that last
wrote a cluster and the build that will open it.

The public product version, Git tag, compatibility identities, and immutable
artifact rules are defined in the [versioning policy](versioning.md). Binary
status and verification commands are on the [download page](../downloads.md).

## Current candidate

| Version | State | PostgreSQL major | Notes |
| --- | --- | ---: | --- |
| [0.1.0a1](0.1.0a1.md) | Unreleased alpha candidate | 19 | First Python wheel and static C SDK |

## Upgrade rule during alpha

The Python API may change before 1.0, and the checked public inventory makes
each change visible to maintainers. The C ABI evolves additively within ABI
major 1 through structure sizes and capability bits.

The PostgreSQL 19 on-disk format is used directly, but the alpha series does
not yet promise that every later PostGamma build will open a cluster created by
an earlier alpha. PostGamma-owned metadata, the embedded safety profile, and
the bundled-extension set may evolve. Unless a later release note explicitly
says direct open is supported:

1. create an in-process logical dump with the old build;
2. keep the original cluster and archive until verification is complete;
3. create a new cluster with the new build;
4. restore the archive and run application checks.

Crossing a PostgreSQL major always requires logical migration. Never copy a
physical cluster into a build with a different PostgreSQL major.

## Artifact verification

Release assets publish wheels and a static SDK archive with checksums. A release
is not identified only by the Python version: record `postgamma.library_info()`
or the C ABI, PostgreSQL major, capability mask, and build ID with the
application artifact.
