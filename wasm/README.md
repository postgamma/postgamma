# PostGamma WebAssembly development

The current commands validate the Wasm toolchain, ABI, pthread/TLS behavior,
stacks, basic libc interfaces, waits, deadlines, cancellation, error recovery,
and OPFS file behavior in Chromium, plus deployment in Chromium, Firefox, and WebKit.
The target build commands also configure PostgreSQL and capture its Wasm
compilation database, generate target state layouts, and link the kernel with
matching initialization resources. The private C/TypeScript startup bridge runs
initdb and SQL in Chromium. The source TypeScript API supports independent
sessions, request cancellation, bounded results and COPY. The OPFS suite covers
ownership, orderly reopen, WAL barriers, storage errors and interrupted recovery.
All four storage groups, including the 18-case Worker/browser recovery matrix,
have passed in debug Chromium.
`make wasm-package-check` builds and validates the versioned browser consumer
tarball. The source package manifest remains private and contains development
tools only; generated consumer metadata has no runtime dependencies.

From the repository root:

```sh
make wasm-setup
make wasm-probe-check
make wasm-platform-check
make wasm-wait-check
make wasm-recovery-check
make wasm-opfs-check
make wasm-browser-setup
make wasm-browser-check
make wasm-resource-check
make wasm-target-check
```

Setup uses the candidate Emscripten version pinned in
[`wasm-emscripten.json`](../manifests/profiles/wasm-emscripten.json), its bundled
Node.js/npm, and the Playwright version locked in `package-lock.json`. It
downloads tools into `.deps/wasm/` and installs npm dependencies in
`wasm/node_modules/`. Repeating setup reuses matching installations; ordinary
builds and checks do not install packages or require a Python virtualenv.

On Ubuntu 24.04 x86-64, missing Chromium libraries are downloaded through the
configured apt repositories and extracted into `.deps/wasm/browser-libs/`.
This does not install system packages or run their maintainer scripts. The
resolved package versions and archive hashes are recorded in that cache.
Other hosts need the browser's system dependencies installed separately;
the doctor reports missing libraries before compiling probes.

| Command | Result |
| --- | --- |
| `make wasm-setup` | Install or reuse the pinned SDK, npm dependencies, Chromium, and required host libraries. |
| `make doctor-wasm` | Validate compiler/package identities and actually launch Chromium. |
| `make wasm-probe-build` | Check the environment and incrementally compile the smoke probe. |
| `make wasm-probe-check` | Build as needed, serve with COOP/COEP headers, and run the probe in Chromium. |
| `make wasm-platform-check` | Run the ABI, pthread, TLS, stack, semaphore, memory stream, entropy, and locale probes. |
| `make wasm-wait-check` | Run wait/notify, deadline conversion and rescheduling, cancellation, close, and stale-event probes. |
| `make wasm-recovery-check` | Run nested error recovery, cleanup, signal-mask, stack, and thread-isolation probes. Requires the PostgreSQL source initialized by `make source-init`. |
| `make wasm-opfs-check` | Exercise the SDK WasmFS OPFS backend, descriptor and namespace semantics, errors, inflight close, and proxy costs. |
| `make wasm-browser-setup` | Install or reuse all three pinned browsers and their headless host libraries. |
| `make wasm-browser-check` | Run shared-memory pthread startup and ten deployment failure scenarios in Chromium, Firefox, and WebKit. |
| `make wasm-resource-check` | Refresh platform, wait, recovery, OPFS, deployment, and capacity evidence; write the provisional resource budget. |
| `make wasm-target-check` | Verify the initial kernel target against the latest complete resource evidence, including source and artifact hashes. |
| `make wasm-kernel-configure` | Configure the target and generate its required headers, with an independently built host timezone generator. |
| `make wasm-kernel-capture` | Compile the selected PostgreSQL objects and static support libraries and capture actual target Clang invocations. |
| `make wasm-kernel-generate` | Generate target state layouts, replacement plans, and PostgreSQL sources with the shared AST tools and ownership rules. |
| `make wasm-runtime-check` | Exercise shared threads, backend execution, notification I/O, timer resampling, main/DSM memory lifetime, and semaphores in Chromium, including TLS destruction and Worker reuse. |
| `make wasm-kernel-link` | Compile the generated kernel, private initdb/libpq and static modules; package target resources and audit final imports and static/TLS sizes. Requires `make wasm-kernel-generate`. |
| `make wasm` | Build the private C/TypeScript startup fixture and Worker assets. Requires `make wasm-kernel-generate`. |
| `make wasm-check` | Build and run target initdb, parameterized SQL, required background roles, shutdown, and two reopens in Chromium. |
| `make wasm-serve` | Build and serve the startup fixture with COOP/COEP headers for manual inspection. |
| `make wasm-sql-check` | Run all eight SQL, concurrency, error, stream, lifecycle, fatal and resource scenarios on one kernel build, with a native SQL/COPY oracle. |
| `make wasm-storage-check` | Run all OPFS ownership, file, durability, storage-error and crash-recovery groups in Chromium. |
| `make wasm-storage-check WASM_STORAGE_SCENARIO=storage` | Check OPFS file identity, exclusive database ownership, in-flight close and orderly SQL reopen. |
| `make wasm-storage-check WASM_STORAGE_SCENARIO=durability` | Check synchronous WAL barriers, storage errors, missing journal handling and the OPFS resource budget. |
| `make wasm-storage-check WASM_STORAGE_SCENARIO=storage-errors` | Inject WAL and namespace failures, discard the failed module and verify PostgreSQL recovery. |
| `make wasm-storage-check WASM_STORAGE_SCENARIO=recovery` | Run the Worker/browser termination matrix, including interrupted initialization and torn namespace-log recovery. |
| `make wasm-vector-check` | Check static pgvector, exact/ANN queries, parallel builds, transactions, independent sessions, persistent recovery and the browser example in Chromium. |
| `make wasm-vector-serve` | Serve the persistent vector search example with the required isolation headers. |
| `make wasm-package` | Stage JS/types, Workers, Wasm, resources, asset manifest and notices; create the npm tarball without publishing it. |
| `make wasm-package-check` | Build the package, install its tarball in a fresh consumer and validate types, SQL/concurrency/persistence and deployment failures. |
| `make wasm-matrix-check` | Measure the last validated package on Chromium, Firefox and Linux WebKit, retaining exploratory failures. |
| `make wasm-clean` | Remove Wasm build output and retain installed tools. |

The smoke probe loads Wasm in a Worker, creates and joins a pthread, and checks
two C function results. The harness also requires cross-origin isolation and
shared Wasm memory. Compilation, browser launch, asset errors, and timeouts
fail the command and retain a report. This is not a complete thread, ABI,
error-recovery, SQL, or persistence test.

The platform probe creates three concurrent pthreads from a precreated pool,
holds them on semaphores and a condition variable while the control Worker
responds to messages, then explicitly wakes and joins them. A second round
checks pool reuse, TLS initialization, `errno`, thread-specific destructors,
thread-local locales, 32/64-bit atomics, and indirect C calls. Stack reports
include requested and actual bounds, live frame usage, and remaining space for
256 KiB, 512 KiB, and 1 MiB pthread stacks.

Two separate module instances deliberately overflow 64 KiB control and pthread
stacks with `STACK_OVERFLOW_CHECK=2`. They must produce the matching stack
diagnostic; a timeout or unrelated exception fails. The browser report retains
expected exceptions and rejects other page or asset errors. These checks measure
the Wasm linear-memory C stack, not the VM call stack or PostgreSQL stack needs.
An allocated guard area is not evidence of native page protection.

The libc checks exercise semaphore counts, cross-thread wakeup and absolute
timeouts, `open_memstream`, `fopencookie` read/write/seek/close callbacks and
deferred write errors. `/dev/urandom` and `getentropy` reach the browser's actual
Web Crypto provider; instrumentation counts calls while retaining the original
random implementation. Sample checks do not assess randomness quality. This
probe enables the SDK's in-memory filesystem, not persistent storage.

Reports retain integration boundaries: unsigned i64 exports require
`BigInt.asUintN(64, value)` at the JS boundary; the pinned SDK rejects
`sem_init(pshared=1)` with a direct `ENOTSUP` return; accepted named locales still
use bytewise `strcoll`. The probes validate these behaviors explicitly.
These platform probes alone do not establish PostgreSQL locale support, kernel
wait/cancel behavior, ERROR recovery, initdb, SQL, or persistence. The separate
kernel startup check below now covers initdb and a limited SQL lifecycle.

The wait probe uses shared request state and Emscripten futex wait/wake in
three precreated pthreads. A 30-second wait is held for about a second while
control messages continue, then explicitly notified. Other cases use infinite
waits. A raw wake without a pending event must not complete a request. Explicit
test gates force notifications before sleep, notifications between predicate
checks and sleep, and timer reset/disarm after the futex has timed out.

Monotonic wait deadlines and PostgreSQL-epoch realtime timer deadlines retain
their own units and origins; each wake recomputes the earlier remaining timeout.
Thirteen clock conversion cases cover expiration, rounding, large differences,
and resampling after a changed wall-clock reading. These last cases inject
input samples into the conversion function; they do not change the system clock.
A separate diagnostic blocks the control Worker's JS event loop while a
pthread deadline completes, checking that it does not depend on a queued JS timer.

The suite also checks three-waiter cancellation isolation, close while blocked,
late request/timer generations, and 64 completion/cancellation races. `WASM_SEED`
selects the reproducible action order and timing inputs for these races; browser
scheduling can still differ. Reports retain the seed, request/timer identities,
nanosecond timestamps as strings, actual futex results, and partial event traces
on failure. Harness timeouts only stop failed runs; they never notify the tested
waiter. This is a candidate platform mechanism, not a PostgreSQL wait adapter
or proof of SQL cancellation, deadlock detection, logical signal dispatch, or
ERROR cleanup. The current PostgreSQL adapter still requires descriptor wakeups.

The recovery probe extracts the actual `PG_TRY`/`PG_CATCH`/`PG_FINALLY` macros
from the target PostgreSQL header into its build directory. The header is an
input to incremental rebuilds; changed extraction boundaries fail explicitly.
The probe supplies its own TLS roots, error throwing, and resource fixtures;
it does not link PostgreSQL's error implementation or execute SQL.

Recovery checks cover nested rethrow, errors during cleanup, typed calls across
translation units, nonzero/zero jump values, linear-memory stack restoration,
and repeated heap allocation/release. Three pthreads keep separate live recovery
boundaries and signal masks across controlled waits and two rounds of thread
reuse. Foreign-thread jump attempts are rejected before accessing a jump buffer.
A separate instance checks that a Wasm trap escapes `PG_TRY` and prevents further
calls into that failed instance while a healthy instance continues.

The pinned SDK aliases `sigsetjmp` to `setjmp`, so even `savemask=1` does not
restore its libc signal mask. The probe records that behavior and tests explicit
restoration of a separate logical mask. Actual PostgreSQL logical interrupt,
transaction rollback, memory-context, and ERROR cleanup integration remains
part of the kernel work.

`WASM_LONGJMP` selects the same explicit setting for every C compilation and
link. The default is `emscripten`; `WASM_LONGJMP=wasm` compares native Wasm
exception handling with `WASM_LEGACY_EXCEPTIONS=0`. These are C recovery probes;
they do not validate C++ exception interoperability or other browser engines.

The OPFS probe uses the SDK's own WasmFS backend with one I/O owner and up to
three C callers. It checks independent and duplicated descriptor offsets,
shared file identity, partial and random reads/writes, truncate, rename/unlink,
explicit flush, directory enumeration, and reopening files in a fresh module.
Probe-only browser wrappers observe real access-handle calls and inject short
transfers, read/quota/flush failures, and two inflight-write/close races. An
owner-side asynchronous directory operation completes while its C caller holds
a probe mutex; the control Worker keeps responding.

The pinned SDK needs an explicit `stringToUTF8OnStack` JS library dependency
for nonempty directory enumeration. The OPFS profile includes it without
editing the SDK. Remaining gaps are reported as `adapter-required`: directory
rename, rename/unlink with an access handle open, truncate with a readonly blob
open, readonly/directory flush, and preservation of quota-specific errors.
A passing probe means those observed behaviors match the checked contract;
`posixStorageComplete` remains false.

Reports contain raw samples and p50/p95 for an empty proxy, 8 KiB reads/writes,
dirty-file flush, and a separate 8 KiB copy calibration with one and three
callers. The empty proxy targets the same owner using a separate queue. Timing
separates the interval before the owner API, the API duration, and the return
interval; it cannot isolate browser-internal copying from storage/cache work.
Cross-Worker clock coarsening can produce small negative boundary deltas,
which are retained. These small cached-file measurements do not establish
database throughput or justify I/O sharding. Orderly reopen after flush does
not test crash recovery, WAL durability, or SQL transactions.

The deployment matrix starts the same shared-memory C module in each browser.
Two rounds keep three pthreads waiting while the control Worker and page remain
responsive, then release and join them with distinct TLS values. Ten fault cases
remove COOP, COEP, both headers, Worker, SharedArrayBuffer, or WebAssembly;
remove the Worker, module, or Wasm asset; or corrupt the Wasm binary. Each must
report its specific error before starting the C probe. The actual HTTP response
headers, browser versions, capability checks, network errors, and per-case
results are retained. Missing browsers are `not-run`; missing required baseline
capabilities are `unsupported`; other launch/runtime errors fail the command.

`make wasm-browser-setup` extends the reusable cache without installing system
packages on Ubuntu 24.04 x86-64. Its Linux WebKit launcher retains the bundle's
own libraries and locates cached EGL vendor data and drivers. Each scenario uses
a fresh browser context. The baseline also records an optional raw OPFS
write/flush/reopen check, separately from the SDK WasmFS probe. Neither a Linux
WebKit result nor this small storage check establishes Safari or SQL support.

Each build uses
`build/wasm/wasm32-emscripten-pthreads/<mode>/<config-id>/`. Configuration changes
select a separate directory; unchanged probe inputs and verified outputs are
reused. Each attempt retains `runs/<run-id>/report.json` and logs.
`reports/smoke.json`, `reports/platform.json`, `reports/wait.json`,
`reports/recovery.json`, `reports/opfs.json`, or `reports/deployment.json` points to the latest attempt,
including failures. Partial browser events are retained when a check fails.
`inputs.json` records source revisions, modified inputs, compiler identity,
flags, and the npm lockfile. Reports distinguish a compiled probe from a
browser pass and always set `kernel_validated` to false.

The resource command runs all six required probes again, reusing compiled
outputs when their inputs match. It never selects an older passing report to
replace a failing attempt. `reports/resource-budget.json` contains the complete
budget and points to its immutable run report. The policy is in
[`wasm-resource-budget.json`](../manifests/profiles/wasm-resource-budget.json).
It reserves a control Worker plus 13 pthread slots (supervisor, six background,
four carriers, I/O owner, and one spare), with 74 MiB of C stacks in total.
The capacity probe starts all slots together, touches synthetic static/TLS
reserves and 144 MiB of allocations, grows shared memory, verifies refreshed
views in JS and pthreads, rejects an excessive allocation with ENOMEM, and
releases allocations across two rounds. The candidate starts at 128 MiB and
allows growth to 512 MiB. The SQL gate now validates these bounds for debug
Chromium and ephemeral WasmFS, including kernel layout, stack depth and managed
JS queue measurements.
Total browser VM/RSS overhead remains unmeasured.
The budget records an initial wasm32/ephemeral decision and conditions that
would require reconsidering target width; it does not validate a Memory64 port.

[`wasm-kernel.json`](../manifests/profiles/wasm-kernel.json) selects wasm32,
Emscripten longjmp and ephemeral WasmFS for the initial kernel port. Memory64
comparison is `not-applicable`: no specific width blocker was observed in the
scoped probes. No wasm64 probe or kernel has run. `make wasm-target-check`
checks the latest budget for the selected mode without rerunning browsers. It
rejects missing, failing, changed or superseded evidence; refresh with
`make wasm-resource-check` if needed. Its report records the selected SDK and
resource policy as future kernel-build inputs. It does not build that kernel.

Single-browser probe profiles stay under each run. Chromium's disposable socket directory
uses a short `/tmp/pgm-wasm-*` path to avoid Unix socket path limits and is
removed after the browser run. Probe files and dependencies remain separate
from native build output.

Useful overrides:

```sh
make wasm-probe-check WASM_MODE=release
make wasm-platform-check WASM_MODE=release
make wasm-wait-check WASM_MODE=release
make wasm-wait-check WASM_SEED=17
make wasm-recovery-check WASM_MODE=release
make wasm-recovery-check WASM_LONGJMP=wasm
make wasm-opfs-check WASM_MODE=release
make wasm-browser-check WASM_MODE=release
make wasm-probe-build WASM_PROBE=platform
make wasm-setup WASM_DEPS_DIR=/opt/postgamma-wasm-tools
make wasm-probe-check WASM_DEPS_DIR=/opt/postgamma-wasm-tools
make wasm-probe-check WASM_EMSDK_DIR=/opt/emsdk
```

`JOBS` bounds SDK compilation parallelism, as it does for native builds.

After `make source-init` and `make wasm-setup`, run `make wasm-kernel-capture`
to prepare the target compiler database. The build uses the same pinned
PostgreSQL source and keeps its configure tree, object files, logs, and
compilation records below `build/wasm/<target>/<mode>/<configuration>/`.
The `zic` generator uses a separate host build below `build/wasm/host-tools/`;
configure does not run target executables. Target reports retain configure
cache answers, explicit platform choices, generated-header hashes, ABI checks,
and object hashes. Repeating the command resumes object compilation and
revalidates the compilation database. A completed capture is not SQL startup
evidence.

`make wasm-kernel-generate` first refreshes the capture, then builds the host
AST tools separately and parses the target compiler commands with the SDK
headers. It checks target pointer, Datum, alignment, callback, and TLS layouts
before generating state storage and transformed sources. Target availability
exclusions are explicit; ownership stays in the shared manifests. AST reuse
requires matching compiler, commands, headers, manifests, and output hashes.
Reports and generated files remain under the selected target configuration;
generation alone does not validate final linking or SQL.

`make wasm-kernel-link` compiles the transformed kernel and its private support
libraries with the target SDK. It also applies the generated-file replacement
plan to parser/scanner C files and builds the registered Snowball and PL/pgSQL
modules with their actual Wasm function signatures. Private frontend symbols
use Clang linkage-name pragmas; native objects and symbol-rewritten native
archives are not inputs. Incremental object reuse checks commands, compiler
identity, dependency contents, and object hashes.

The target resource pack contains matching catalogs, SQL, configuration,
timezone data, and static-module metadata under `/postgamma`. Its `bin/postgres`
file is a resource-location anchor used by the shared C entry point. Resources
contain no native executable or preinitialized database. Link receipts, object
references, final imports, exported static/TLS measurements, and the linker
map are saved in `runs/link-*/`; `reports/link.json` points to the latest result.
Unknown final imports and stale generated inputs fail the command. Retained
address compatibility branches are identified separately from the supported
memory transport. Linking does not establish browser startup or persistence.

After target generation, run:

```sh
make wasm-check JOBS=2
```

The C bridge creates a database in the module's memory filesystem, opens a
pooled threaded instance, executes `SELECT $1::int4` with a bound parameter,
and closes it. Two further rounds reopen the same database. Each round checks
the checkpointer, background writer and WAL writer, a static Snowball call,
division-by-zero recovery, and rejection of an unavailable libc locale.
Directory claims, main/DSM segments and notification pipes must be released.
The report checks each instance's role and stack accounting, confirms all
SDK pthreads have returned to the pool, and then disposes the idle Worker pool.
Both the page and control Worker must remain responsive during the run.

`make wasm` builds the same fixture without claiming SQL validation;
`make wasm-serve` prints a local URL for manual inspection. The pinned Node
runtime strips the private TypeScript sources; browser execution validates the
result, without adding a separate TypeScript compiler dependency. Startup
reports are saved beside link reports, with `reports/startup.json` pointing to
the latest result. Repeating a check reuses matching target objects and runs a
fresh module. If runtime/manifests/headers changed, refresh generation first.

This is an initial debug-mode Chromium check, not the public JavaScript SDK.
It does not establish concurrent SQL coverage, durable OPFS storage, release
kernel validation, or a full browser matrix. `wasm-check-full` remains unavailable
until those later checks are implemented. The SQL gate below validates its
resource budget separately; linear memory's high-water size need not shrink when an instance closes.

`WASM_EMSDK_DIR` selects an already activated SDK matching the profile's
Emscripten version; setup does not install or activate that SDK. `WASM_NODE` can select a
compatible Node executable, and `WASM_BROWSER_EXECUTABLE` an explicit Chromium
executable. The runner and actual browser version are recorded. The default
uses the managed tools without sourcing `emsdk_env.sh` or changing your shell.

`BUILD_DIR` controls disposable output. Dependency and SDK paths inside it
are rejected, including symlinked paths. Both `make wasm-clean` and the root
`make clean` retain `.deps/wasm/` and `wasm/node_modules/`. Cleaning build output
therefore does not require downloading the toolchain again.

`make wasm-sql-check JOBS=2` builds the asynchronous SQL bridge once and runs
all eight stage-2 browser scenarios. Use `WASM_SQL_SCENARIO=abi` to select only
parameters, exact text values, NULL, binary data, diagnostics and stale handles.
Results are recorded in `reports/sql.json`. The source API is
under `src/api/`, exported by `src/index.ts` in the consumer package. Result
buffers crossing the Worker boundary own their bytes and do not borrow Wasm
memory. `WASM_SQL_SCENARIO=concurrency` checks real lock waits, independent session
state and progress under pool saturation. `WASM_SQL_SCENARIO=errors` checks
request cancellation, SQL timeouts and real crossed-row deadlocks. Bounded
streaming and COPY are checked with `WASM_SQL_SCENARIO=results`, including a
native PostgreSQL oracle (requires the existing `build/reference/tmp_install`
installation). Iterator/COPY delivery grants one batch of credit at a time.
Collected queries are limited to 4 MiB; larger results use async iteration.
Exact text values stay strings, binary values stay owned byte buffers, and
SQL errors expose SQLSTATE plus the native diagnostic fields.

The setup follows the [Emscripten SDK instructions](https://emscripten.org/docs/getting_started/downloads.html),
and the harness uses the [Playwright library](https://playwright.dev/docs/library).

`WASM_SQL_SCENARIO=lifecycle` exercises close during lock waits, result/COPY
backpressure and handle submission. `fatal-trap` and `fatal-abort` exercise real
test-only faults and fresh-module recreation. Normal close waits for native
resource and pthread reclamation. Fatal teardown requests Worker termination
and discards the failed module; the browser does not provide synchronous VM
termination acknowledgement. These checks cover ephemeral databases in the
debug Chromium profile.

The accepted SQL limits are generated for both C and JavaScript from
`manifests/profiles/wasm-resource-budget.json`: four live connections, 256 KiB
native result buffers, 128 KiB values, 16 KiB COPY fragments, 2 MiB pending
query bytes per connection, and 8 MiB queued Worker messages. `budget` checks
all four carriers and admission/queue limits. The full run writes
`sql-resource-budget.json` beside its `sql.json`; the latter includes all
scenario results and source/asset identities. Startup samples run at 1 ms;
the 13-slot SDK pool supplies the hard pthread bound. Caller-retained results
and the browser's VM/code memory are outside managed queue accounting.

A minimal source-API call, from a page serving the generated assets with
COOP/COEP headers, is:

```typescript
import {Database} from './src/api/database.js';
const db = await Database.open();
const connection = await db.connect();
const result = await connection.query('SELECT $1::int8', {
  parameters: [{typeOid: 20, value: '9223372036854775807'}],
});
console.log(result[0].rows[0][0]); // Exact string
await db.close();
```

Use `for await (const batch of await connection.execute(sql, {chunkRows: 128}))`
for larger results. Each connection serializes `query()` calls; explicit request
objects expose a single-consumer iterator and `cancel()`/`close()` methods.
Opening another connection can return `DatabaseError` with BUSY status when
pinned sessions and in-flight requests reserve the available carriers. Retry
after existing work releases capacity; existing connections can still commit
or roll back their transactions.

The OPFS source API identifies a database by its origin and name:

```typescript
const db = await Database.open({storage: {type: 'opfs', name: 'example'}});
const connection = await db.connect();
await connection.query('CREATE TABLE IF NOT EXISTS items (id integer PRIMARY KEY)');
await db.close();
const reopened = await Database.open({storage: {type: 'opfs', name: 'example'}, create: false});
await reopened.close();
```

Names contain 1-64 ASCII letters, digits, underscores or hyphens. A second owner
of the same database is rejected before initialization/recovery. Ownership lasts
through native shutdown and I/O drain. The provider uses a dedicated 2 MiB-stack
I/O thread, at most 64 cached physical access handles, and stable file identities
behind a checksummed namespace journal. The initial journal is bounded to 16 MiB
and the namespace to 16,384 live entries. The profile does not provide symlinks
or external tablespaces. It requires OPFS and Web Locks and never falls back to
an ephemeral database when persistent storage cannot be opened.

Storage cases include cold target initdb and reopen with a separate 90-second
case deadline and record the observed duration. The durability case keeps fsync
and synchronous commit enabled, pauses actual WAL flush before COMMIT acknowledgement
and lock release, and checks unrelated session progress. Short writes, quota,
file/directory sync and teardown failures propagate to callers. A missing namespace
journal rejects opening a nonempty store and preserves its data files.

The storage-errors case verifies that a WAL flush failure cannot acknowledge
COMMIT and that uncertain namespace writes require a fresh module. Recovery must
retain acknowledged commits and transaction atomicity; a transaction whose reply
was lost may already be committed. The OPFS resource checks include the measured
2 MiB I/O stack, existing 13-Worker pool, 512 MiB linear memory maximum, bounded
queues, 64 data access handles plus one journal handle, and healthy shutdown drain.
Browser VM/RSS and hardware power-loss durability are not measured.

The recovery group uses one origin and persistent browser profile. Each of
Worker termination and SIGKILL of the harness-owned browser process group is
injected before/after WAL flush, during checkpoint data writes, file creation,
relation extension, before/after namespace rename flush, and after flushing a
partial namespace record. Both also interrupt target initdb. The runner verifies
PostgreSQL automatic recovery, acknowledged transaction pairs, DDL/extension
rollback, file identity and actual journal-tail replay. Its complete 18-case
matrix has a 600-second watchdog and a 90-second limit per recovery operation;
each receipt retains gates, interruption method, browser process identity,
recovered SQL, logs and resource measurements. Progress is saved periodically
so failed runs retain the last reached boundary and startup phase.
The process-group termination harness currently requires Linux `/proc`.

`make wasm-vector-check JOBS=2` compiles the pinned pgvector 0.8.6 source with
the shared extension state policy and the actual wasm32 ABI. The extension
variant has separate generated sources, objects, resources and linked assets
under `<target-build>/variants/<extension-id>/`; the base kernel output remains
available. CPU dispatch and Wasm SIMD are disabled for this scalar profile.
The static registry includes the exact SQL and parallel-worker entry signatures,
matching control/SQL resources, and the upstream license in the resource pack.
The complete gate runs five groups: base ABI with the extension linked,
extension loading/scalar functions, vector queries, persistent vector indexes,
and the browser example. Use `WASM_VECTOR_SCENARIO=vector-load`, `vector-query`,
`vector-storage`, or `vector-example` to rerun one group.

The query group reuses the native 2,000-row dataset and pinned upstream L2,
NULL, insert and truncate fixtures. It checks actual HNSW/IVFFlat plans and
distance ordering, with a workload-specific minimum recall of 8/10 against
exact neighbors. Both index builds must actually launch and reclaim one
parallel worker. Transactions, DDL rollback, SQL error reuse and four concurrent
sessions with independent extension settings are checked separately.
The OPFS group checks orderly reopen, committed indexed rows, Worker termination
before WAL acknowledgement, atomic recovery of a cross-index transaction and
index queries after VACUUM. The example uses parameterized SQL and the same
three documents as the Python vector search example.

Run `make wasm-sql-check JOBS=2` first to create a passing base-kernel receipt
for the resource comparison. The full vector gate records matching core input
identity, static/TLS and asset-size deltas, the repeated ABI memory measurement,
and separate workload peaks in `vector-resource-budget.json`. These checks use
the existing 13-Worker and 512 MiB limits. They cover debug Chromium; browser
VM/RSS, hardware power loss and the full upstream pgvector regression suite
are outside this measured scope.

`make wasm-sql-check WASM_EXTENSIONS=pgvector JOBS=2` runs the original SQL gate
against the extension variant. The storage gate accepts the same variable.
Use `make wasm-vector-serve JOBS=2` and open the printed URL to try
`examples/wasm/vector-search.html`; close and reopen the database to reuse its
documents. The development server supplies the required isolation headers.
The versioned consumer package is built separately with `make wasm-package-check`.

## Browser consumer package

```sh
make wasm-kernel-generate JOBS=2
make wasm-package-check JOBS=2
```

The package report names the staged directory and npm tarball under the selected
variant's `package/<build-id>/` and `dist/`. Install that tarball with
`npm install /path/to/postgamma-wasm-0.1.0-alpha.1.tgz`. The package defaults to
static pgvector; `WASM_PACKAGE_EXTENSIONS=none` selects the extension-free build.
No command publishes to a registry.

The independent consumer installs with lifecycle scripts disabled, resolves
the package exports and generated declarations, and serves only its own files.
It checks parameters, transactions, four actual concurrent sessions, cancellation,
HNSW/IVFFlat, OPFS reopen and an explicitly relocated asset directory. Negative
cases reject mismatched build/protocol identities, corrupt Wasm/resource bytes,
a missing Worker and missing cross-origin isolation before reporting success.

`postgamma.assets.json` and generated loader identity bind the API to its private
bridge and exact runtime assets. The loader verifies core asset hashes before
instantiation; the package gate checks every declared asset, notices, version and
absence of checkout paths. TypeScript is a pinned development dependency only.
Follow the [consumer guide](PACKAGE_README.md) to copy the complete asset tree,
set HTTP headers and use `assetBase` with a bundler. Runtime browser support and
performance must be reported for the actual tested profile.
