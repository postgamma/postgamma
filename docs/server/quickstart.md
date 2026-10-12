# Server quickstart

Build and run the threaded PostgreSQL server from a complete source checkout.
Use the [installation guide](../getting-started/installation.md) for compiler and
system dependencies. These commands target Linux and run from the repository
root, in the same shell, as a regular operating-system user. PostgreSQL refuses
to initialize or run a server as root.

## Build and install

```bash
git submodule update --init --recursive
make doctor-threaded JOBS=4

POSTGAMMA_SERVER_PREFIX="$PWD/build/server-install"
make threaded-postgresql-build JOBS=4 \
  PG_GENERATED_CONFIGURE_ARGS="--prefix=$POSTGAMMA_SERVER_PREFIX"
make -C build/generated-build install
```

The generated source is in `build/generated/postgres/`; the compiled tree is in
`build/generated-build/`. The install step writes the server, client tools,
libraries, and resources below `POSTGAMMA_SERVER_PREFIX`. It needs no system-wide
installation or sudo. Keep the prefix consistent when rebuilding, and modify
PostGamma sources rather than the generated tree.

This is a developer installation. Compiling and installing it does not run the
[full validation suite](compatibility.md#validate-the-threaded-runtime) or create
a distributable server package.

## Initialize a fresh cluster

Choose a new data directory and a database password when prompted:

```bash
POSTGAMMA_SERVER_DATA="$PWD/build/server-data"
"$POSTGAMMA_SERVER_PREFIX/bin/initdb" \
  -D "$POSTGAMMA_SERVER_DATA" -U postgamma \
  --encoding=UTF8 --locale=C \
  --auth-local=peer --auth-host=scram-sha-256 --pwprompt
```

The example connects over loopback TCP using password authentication. Local
Unix-socket connections use peer authentication. Keep the data directory separate
from an embedded PostGamma cluster or an existing PostgreSQL installation.

## Start and connect

Port 55432 must be available. The example listens only on the local machine:

```bash
"$POSTGAMMA_SERVER_PREFIX/bin/pg_ctl" \
  -D "$POSTGAMMA_SERVER_DATA" -l "$PWD/build/server.log" \
  -o "-h 127.0.0.1 -p 55432" -w start

"$POSTGAMMA_SERVER_PREFIX/bin/psql" \
  -h 127.0.0.1 -p 55432 -U postgamma -d postgres
```

At the psql prompt:

```sql
CREATE DATABASE app;
\connect app
CREATE TABLE notes (id bigint PRIMARY KEY, body text);
INSERT INTO notes VALUES (1, 'threaded PostgreSQL');
SELECT * FROM notes;
\quit
```

Other PostgreSQL clients use the same host, port, role, and database. For example,
a libpq connection string is
`host=127.0.0.1 port=55432 user=postgamma dbname=app`; supply the password through
the client's password prompt or credential mechanism.

## Stop

```bash
"$POSTGAMMA_SERVER_PREFIX/bin/pg_ctl" \
  -D "$POSTGAMMA_SERVER_DATA" -m fast -w stop
```

Fast shutdown disconnects clients, rolls back their active transactions, and
shuts down the cluster cleanly. Restart with the same start command to reopen
the stored data. Continue with [server operations](operations.md).
