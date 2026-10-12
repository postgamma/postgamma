# Server operations

This guide applies to the standalone threaded server. Python and C embedded
instances use their API lifecycle and management operations. Browser instances
use the [browser SDK](../getting-started/browser.md).

The examples reuse `POSTGAMMA_SERVER_PREFIX` and `POSTGAMMA_SERVER_DATA` from the
[quickstart](quickstart.md), with a server listening at `127.0.0.1:55432`.

## Configuration and connections

The cluster contains PostgreSQL's `postgresql.conf` and `pg_hba.conf`. Configure
server settings and client authentication there. The quickstart's `-h` and `-p`
startup options override the corresponding file settings; change those options
when changing the listener.

Use `pg_ctl reload` for settings that support reload. Settings requiring restart
take effect only after a server restart:

```bash
"$POSTGAMMA_SERVER_PREFIX/bin/pg_ctl" -D "$POSTGAMMA_SERVER_DATA" reload
"$POSTGAMMA_SERVER_PREFIX/bin/pg_ctl" -D "$POSTGAMMA_SERVER_DATA" -w restart
```

For access beyond localhost, configure the listener, authentication, and TLS
for that deployment. The quickstart configures loopback access only. TLS support
depends on the PostgreSQL build options and their system dependencies; see the
[configure probes](../getting-started/installation.md).

## Lifecycle and resources

```bash
"$POSTGAMMA_SERVER_PREFIX/bin/pg_ctl" -D "$POSTGAMMA_SERVER_DATA" status
"$POSTGAMMA_SERVER_PREFIX/bin/pg_ctl" -D "$POSTGAMMA_SERVER_DATA" -m fast -w stop
```

Use the server log selected at startup to diagnose startup failures and recovery.
After an interrupted run, start the same cluster through `pg_ctl` and allow
PostgreSQL WAL recovery to finish before accepting application traffic. Recovery
behavior belongs in the validation of the actual server build and storage setup.

All roles share the server process's file-descriptor limit. Size the process
limits for the configured connections and workload. The full source validation
suite has its own [higher descriptor requirement](../getting-started/installation.md#threaded-server-process-limits).

## Logical backup and restore

Use the client tools installed with this build. For the `app` database created
by the quickstart, make a custom-format logical archive:

```bash
"$POSTGAMMA_SERVER_PREFIX/bin/pg_dump" \
  -h 127.0.0.1 -p 55432 -U postgamma -d app \
  --format=custom --file=app.dump
```

Restore it into a new database and verify it before replacing application data:

```bash
"$POSTGAMMA_SERVER_PREFIX/bin/createdb" \
  -h 127.0.0.1 -p 55432 -U postgamma app_restored
"$POSTGAMMA_SERVER_PREFIX/bin/pg_restore" \
  -h 127.0.0.1 -p 55432 -U postgamma \
  --dbname=app_restored --exit-on-error app.dump
"$POSTGAMMA_SERVER_PREFIX/bin/psql" \
  -h 127.0.0.1 -p 55432 -U postgamma -d app_restored \
  -c 'SELECT * FROM notes;'
```

A database dump does not include cluster-wide roles or tablespaces; inventory
those separately when planning a migration. Retain the source cluster until
schema, data, permissions, and application checks succeed. Physical backup,
replication, and direct directory reuse require their own
[compatibility assessment](compatibility.md).
