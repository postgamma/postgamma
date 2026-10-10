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
Firefox and WebKit engines, using normal browser profiles. Native Firefox resource
runs place the browser and localhost asset server in a loopback-only Linux network
namespace. This excludes unrelated browser downloads and remote configuration
updates without overriding browser preferences, GC or memory limits. The runner
requires util-linux `unshare` and permission to create a network namespace, using
an unprivileged user namespace when run without root. It fails if isolation is
unavailable. Other functional tests retain their existing networking.
Safari on macOS/iOS
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
One contributor is a confirmed WebKit native Worker leak: the bundled WebKit
2359 does not own or destroy each VM's `JSHeapData`. WebKit corrected this in
[322643@main](https://github.com/WebKit/WebKit/commit/6cf979a79f7bb78c0635ff43f02282d78137583e).
An isolated browser ownership backport validates that cause; it is not a shipping
SDK repair. A second confirmed WebKit leak, the missing `adoptRef` in
`WorkerUserGestureForwarder::create`, retains an object on each main-thread
`Worker.postMessage`. The SDK avoids that path with child-owned MessagePort
mailboxes for both control and pthread Workers; bootstrap, transferred data,
OPFS ports and retirement are validated together. Long-run memory growth still
requires qualification under the original limits. Linux native Worker memory diagnostics and
mixed-load tests use a
SHA256-pinned unmodified upstream WPE 322662@main build containing the repair;
`make wasm-webkit-setup` prepares that browser. Its mixed-load driver preserves
the workload, OPFS/close checks and resource limits, and measures actual Linux
Worker threads for the ten-second retirement check. Playwright functional gates
still use their separate affected bundle. Native diagnostics and short mixed-load
checks do not complete long-run acceptance.
The SDK creates each fresh shared memory at zero pages, grows it to the declared
initial size, and validates its buffer in the allocating realm before Worker
startup. In the pinned WPE build,
[`JSWebAssemblyMemory::finishCreation`](https://github.com/WebKit/WebKit/blob/9eb4e2ad5983af0d494b62b38e55ff9010fb5993/Source/JavaScriptCore/wasm/js/JSWebAssemblyMemory.cpp)
accounts the empty backing store before the real allocation is adopted.
Materializing the buffer alone does not register the Memory wrapper's missing
allocation; growing it invokes `growSuccessCallback`, which reports that cost.
A 40-allocation native reproduction with no forced GC reduced peak outstanding
linear memory from 384 MiB to 128 MiB with this initialization sequence.
This addresses delayed collection; it does not establish full endurance
acceptance or classify every remaining browser allocation.
Run `make wasm-soak-test` to validate two
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
