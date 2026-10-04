# Browser developer preview 0.1.0-alpha.1

Status: unreleased; long-run resource acceptance is blocked and npm publication
is pending. The version is derived from the repository's `0.1.0a1` product
version. This page describes the browser
package; the native Python and C SDK have separate acceptance requirements.

## Included

- PostgreSQL 19 SQL and transactions in browser Workers, with four independent
  connections sharing one database;
- parameterized queries, bounded result iteration, COPY streams, cancellation,
  SQLSTATE and structured errors;
- ephemeral databases and OPFS persistence, WAL storage barriers, reopen and
  interruption recovery;
- static pgvector 0.8.6, including HNSW and IVFFlat indexes;
- static pg_trgm 1.6 and hstore 1.8, including their indexes;
- complete same-build data-directory export, restore into a new target and clone;
- connection startup, cancellation, request release and COPY/connection close
  that yield during lifecycle waits so other connections can continue;
- an independently installable package containing JavaScript, TypeScript
  declarations, Workers, Wasm, initialization resources and license notices.

## Accepted scope and deployment

The tested platform is Linux x86-64 with the project's pinned Chromium,
Firefox and WebKit engines, using normal browser profiles. Safari on macOS/iOS
and mobile devices remain unaccepted. Node.js, WASI, multiple live instances
and arbitrary native extension loading are outside this preview's profile.

Applications need HTTPS or localhost, cross-origin isolation, same-origin
module Workers and a complete copy of the package's assets. OPFS additionally
needs Web Locks, BroadcastChannel and synchronous access handles. Follow the
[browser guide](../getting-started/browser.md) for installation, headers,
asset placement and a runnable query example.

The initial shared linear memory is 128 MiB and its ceiling is 512 MiB;
JavaScript heaps, compiled code and browser memory are additional. The pthread
pool has 13 slots. Collected query results are limited to 32 MiB including
metadata, and native materialized results to 24 MiB; use result iteration for
larger results. Individual values are limited to 16 MiB.

## Data and lifecycle boundaries

OPFS data belongs to the origin and database name. One live owner can open that
database at a time. Site-data deletion removes the database; changing origins
does not migrate it. A native PostgreSQL data directory cannot be mounted.

Successful synchronous COMMIT waits for browser storage barriers. Interruption
tests do not establish hardware power-loss durability. The full storage-error
and crash-recovery suite is a Chromium gate; the three-engine package matrix
also covers persistent reopen.

Storage metadata checkpoints bound namespace history. Opening an older preview
database can upgrade its metadata, requiring up to 5 MiB of additional space;
older builds cannot reopen that upgraded format. This metadata migration is
not a general PostgreSQL major-version upgrade. `dumpDataDir()` closes an idle
source and streams a complete archive. `Database.loadDataDir()` requires the
exact matching asset build and a new target; `clone()` combines these steps.
Failed OPFS restores retain an incomplete marker and cannot be opened. This
archive is not a cross-version migration format.

Close requests, COPY streams and connections before closing the database.
Lifecycle timeouts retain objects that are still referenced and allow close
to be retried. Connection startup starts instance discard at 90 seconds if it
cannot complete or retire. Caller rejection follows the cleanup attempt;
existing connections on that instance become unusable. Cleanup failures remain
visible in errors and diagnostics.
After a fatal instance failure, discard that instance and reopen the database.

## Release acceptance

Long mixed-load runs currently exceed the 128 MiB browser memory-growth budget.
The cause is still under investigation. Run `make wasm-soak-test` to validate two
hours in each engine, including clean reopen, trap/abort recovery, cancellation
and unchanged resource limits. Short regressions and forced-GC diagnostics do
not complete this acceptance.

Publish only after the final commit's Browser kernel workflow passes the PG19
optimized browser suite, shared native runtime/COPY/event regression, and the
fixed PG20 candidate suite. The PG20 result applies only to candidate
`7a0aa99e51c7a3263e6ee45f19f270ddae430a92`; it is not a claim about later upstream
commits. Physical Safari/mobile acceptance is a separate milestone.

The publishable archive must pass independent package installation, match its
asset manifest and retain all notices. Publish this prerelease with npm tag
`next`, preserving the archive's hash and source identity. After publication,
install that exact registry version in a fresh consumer and verify SQL,
OPFS reopen, pgvector and shutdown. A dry run is not a registry release.
