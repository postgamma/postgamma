# PostgreSQL compatibility

PostgreSQL 19 is the only supported kernel and on-disk cluster major in the
current release.

## Preserved behavior

PostGamma reuses PostgreSQL's parser, catalogs, planner, executor, transaction
manager, WAL, crash recovery, SQL type system, simple protocol, extended
protocol, and logical dump/restore implementations. Behavioral gates compare
ordinary PostgreSQL/libpq execution with the in-process path.

## Cluster directories

A PostGamma database path is a PostgreSQL cluster directory with additional
embedded ownership rules. It is not a single-file format. PostGamma does not
write a signalable host PID into `postmaster.pid`, change the host working
directory, or permit two owners to open the same directory concurrently.

Do not run `pg_ctl` against a live embedded cluster. Use the PostGamma API for
lifecycle and management.

## Major upgrades

PostgreSQL data directories are not portable across major versions. To migrate:

1. logically dump through the old supported PostGamma build;
2. create a fresh instance with the new PostgreSQL-major build;
3. logically restore the archive;
4. run application-level verification before replacing the old directory.

Upgrade-audit support in the source repository measures how much adapter and
ownership policy changes for a candidate PostgreSQL revision. Passing such an
audit does not advertise candidate-major product support.

During the alpha series, direct cluster compatibility between PostGamma builds
is promised only when the destination release notes say so. Otherwise use the
same logical dump and restore procedure even when both builds embed PostgreSQL
19. See [Releases and upgrades](../releases/index.md).

## Extensions

Only extensions named in the build's reviewed bundled-extension manifest are
available. An ordinary binary PostgreSQL extension is not ABI-compatible merely
because its SQL and C sources compile against PostgreSQL 19. See the
[bundled extension SDK](../extensions/index.md).

The first release includes pgvector 0.8.6. See
[Vector search with pgvector](../extensions/pgvector.md) for the installation,
index, parameter, and result contracts.
