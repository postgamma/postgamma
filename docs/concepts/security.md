# Security model

PostGamma is an in-process database library, not a security sandbox. PostgreSQL
code, SQL execution, native extensions, and the host application share one
address space and the same operating-system identity.

## Trust boundary

Treat these inputs as trusted:

- the PostGamma wheel or static SDK;
- the matching runtime resource pack;
- every bundled native extension;
- the cluster directory and its contents; and
- SQL accepted from callers with powerful PostgreSQL privileges.

Do not open a cluster directory supplied by an untrusted party in a sensitive
host process. A malformed database or a native-code defect is inside the host's
fault boundary.

## SQL and filesystem authority

The initial `postgamma` role created with a cluster is a PostgreSQL superuser.
Server-side PostgreSQL file functions and `COPY` with a filename therefore run
with the host process's filesystem permissions. Do not expose unrestricted SQL
from that role to an untrusted user. Create narrower PostgreSQL roles and grant
only the objects and operations the application requires.

The embedded profile rejects shell-backed `system()`, `popen()`, and
`COPY ... PROGRAM` execution instead of launching a child process. That does
not make arbitrary SQL safe: file reads and writes, resource exhaustion, native
functions, and application-defined codecs remain part of the trusted boundary.

## Network and authentication boundary

PostGamma opens no PostgreSQL listener, TCP socket, Unix-domain SQL socket, or
`pg_hba.conf` endpoint. A Python or C caller that can reach a live handle is
already inside the application trust boundary. PostgreSQL roles still control
SQL privileges and object ownership, but they do not authenticate an operating-
system client connection.

## Runtime resources and extensions

Deploy the resource pack from the same build as the library, keep it read-only
where practical, and do not search writable shared directories for it. Replacing
bootstrap SQL, catalogs, time-zone data, or bundled extension resources changes
the code and data trusted by database startup.

Arbitrary PostgreSQL shared objects cannot be loaded. Native extensions must be
reviewed, compiled into the product, and declared in the bundled-extension
manifest. The extension capability model does not sandbox native code.

## Data at rest

PostGamma does not provide transparent encryption for the cluster directory or
logical archives. Use operating-system permissions and, when required, volume,
filesystem, or application-managed encryption. Protect backups separately from
the live cluster.

## Fork and crash scope

Handles inherited across `fork()` fail closed. A native memory-safety failure,
PostgreSQL `PANIC`, or undefined behavior may terminate the host application;
there is no process boundary to contain it. WAL recovery protects storage
semantics, not host-process availability.

Report suspected vulnerabilities through the private channel described in the
repository's `SECURITY.md` policy.
