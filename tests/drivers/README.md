# External PostgreSQL drivers

This optional check runs unmodified pgJDBC, Psycopg, pg8000, and libpq clients
against both the original PostgreSQL process server and the generated PostGamma
thread server. Each server uses an isolated database and SCRAM authentication
over loopback TCP. It does not exercise the in-process C/Python API.

Supply an installed reference server, an installed thread server, a pgJDBC JAR,
a JDK 11 or later, and a Python interpreter containing `psycopg[binary]` and `pg8000`:

```sh
python3 buildsys/check_external_drivers.py \
  --reference-prefix /path/to/original/postgres \
  --thread-prefix /path/to/thread/postgres \
  --jdbc-jar /path/to/postgresql.jar \
  --java /path/to/jdk/bin/java --javac /path/to/jdk/bin/javac \
  --python /path/to/drivers-venv/bin/python \
  --output build/external-drivers-check
```

Use a new output directory for each run. The checker compiles the Java consumer
with the Java 8 API baseline, retains driver output and server logs, and records
client versions plus JAR, fixture, and server binary hashes in `report.json`.
It starts and stops only the clusters it creates and preserves a cluster if
shutdown fails. It never installs dependencies or modifies existing databases.

JDBC covers typed parameters and prepared/binary results, transactions and
savepoints, generated keys and batches, cursor fetching, COPY, LISTEN/NOTIFY,
simple query mode, driver query timeout, explicit CancelRequest, four concurrent
sessions, and invalid credentials. The Python clients cover typed parameters,
transactions, batches, concurrent session isolation, timeout recovery, invalid
credentials, and connection churn. Psycopg also covers protocol preparation and
explicit cancellation. The libpq consumer checks SCRAM and UTF8.

This is bounded compatibility evidence, not the upstream drivers' full test
suites. TLS, GSS, replication clients, connection poolers, and other drivers
are outside this check.
