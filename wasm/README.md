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
All four storage groups, including the 32-case Worker/browser recovery matrix,
have passed in debug and release Chromium.
`make wasm-package-check` builds and validates the versioned browser consumer
tarball. The source package manifest remains private and contains development
tools only; generated consumer metadata has no runtime dependencies.

From the repository root:

```sh
make wasm-test
```

This is the one-command local Wasm acceptance entry point. It initializes the
pinned source, installs/reuses the pinned SDK and browsers, builds in release
mode, and runs the existing SQL/vector/storage/lifecycle/package gates plus the
PGlite-derived regression matrix. Its default build width is one job;
`JOBS=2` (or `WASM_TEST_JOBS=2`) explicitly changes it. Existing
`WASM_EMSDK_DIR` and `WASM_DEPS_DIR` overrides still work. Host C/C++/LLVM and
PostgreSQL build prerequisites are checked by the existing doctor.

Independent stages continue after a test failure. A failed prerequisite marks
its dependent stages `not-run`; any failure or unexecuted stage makes the final
command exit nonzero. The summary pointer is
`build/wasm/reports/ci.json`, and its `run_report` names the complete stage
report and sibling logs. No command publishes a package. This is Wasm
acceptance, not the separate full native PostgreSQL product regression suite.

For individual checks:

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
| `make wasm-generator-tools-check` | Check that the selected host AST frontend parses the pinned SDK's real integer and errno headers before compiling the kernel. |
| `make wasm-runtime-check` | Exercise shared threads, backend execution, notification I/O, timer resampling, main/DSM memory lifetime, and semaphores in Chromium, including TLS destruction and Worker reuse. |
| `make wasm-kernel-link` | Compile the generated kernel, private initdb/libpq and static modules; package target resources and audit final imports and static/TLS sizes. Requires `make wasm-kernel-generate`. |
| `make wasm` | Build the private C/TypeScript startup fixture and Worker assets. Requires `make wasm-kernel-generate`. |
| `make wasm-check` | Build and run target initdb, parameterized SQL, required background roles, shutdown, and two reopens in Chromium. |
| `make wasm-serve` | Build and serve the startup fixture with COOP/COEP headers for manual inspection. |
| `make wasm-sql-check` | Run SQL, binding, concurrency, error, stream, lifecycle, OPFS failure recovery, fatal and resource scenarios on one kernel build, with a native SQL/COPY oracle. |
| `make wasm-pglite-check WASM_MODE=release` | Run the pinned PGlite-derived cases in Chromium, Firefox and Linux WebKit, each with ephemeral and OPFS storage. Requires setup and `wasm-kernel-generate` for this mode. |
| `make wasm-soak-test` | Prepare sources, SDK, browsers and a release build, then run the seeded mixed workload for two hours per browser (about six hours total). |
| `make wasm-soak-check WASM_MODE=release` | Run the same endurance gate using an already prepared SDK and generated kernel. |
| `make wasm-alpha-test` | Prepare tools, pass all release gates, then qualify the exact production npm archive for two active hours per browser. |
| `make wasm-alpha-check WASM_MODE=release` | Run the same complete alpha qualification using already installed tools. |
| `make wasm-lifecycle-check` | Run connection close progress/deadline and OPFS close/startup error recovery in Chromium, Firefox and Linux WebKit, plus 20 consecutive trap and 20 abort/reopen cycles per engine. Use `WASM_LIFECYCLE_REPETITIONS=N` to change the count. |
| `make wasm-storage-check` | Run all OPFS ownership, file, durability, storage-error and crash-recovery groups in Chromium. |
| `make wasm-storage-check WASM_STORAGE_SCENARIO=storage` | Check OPFS file identity, exclusive database ownership, in-flight close and orderly SQL reopen. |
| `make wasm-storage-check WASM_STORAGE_SCENARIO=durability` | Check synchronous WAL barriers, storage errors, missing journal handling and the OPFS resource budget. |
| `make wasm-storage-check WASM_STORAGE_SCENARIO=storage-errors` | Inject WAL and namespace failures, discard the failed module and verify PostgreSQL recovery. |
| `make wasm-storage-check WASM_STORAGE_SCENARIO=recovery` | Run the Worker/browser termination matrix, including interrupted initialization and torn namespace-log recovery. |
| `make wasm-vector-check` | Check static pgvector, exact/ANN queries, parallel builds, transactions, independent sessions, persistent recovery and the browser example in Chromium. |
| `make wasm-vector-serve` | Serve the persistent vector search example with the required isolation headers. |
| `make wasm-package` | Stage JS/types, Workers, Wasm, resources, asset manifest and notices; create the npm tarball without publishing it. |
| `make wasm-package-check` | Build the package, install its tarball in a fresh consumer and validate types, SQL/concurrency/persistence and deployment failures. |
| `make wasm-ci` | Run tool/unit/generation/shared-runtime/native-oracle/SQL/package CI stages locally and retain stage timings and diagnostics. |
| `make wasm-test` | Set up and run all release Wasm gates, including the PGlite-derived matrix; retain independent failures in one report. |
| `make wasm-release` | Build an optimized package and run SQL, vector, PGlite-derived, storage, lifecycle, package and browser gates. |
| `make wasm-check-full` | Run the pinned source through platform and shared-runtime probes, SQL, vector, OPFS recovery, installed package and browser matrix gates. |
| `make wasm-weekly` | Run the full suite against current PostgreSQL master in an isolated candidate checkout; `PG_BRANCH` and `PG_REPOSITORY` are configurable. |
| `make wasm-matrix-check` | Validate and measure the last package on Chromium, Firefox and Linux WebKit; every engine must pass. |
| `make wasm-clean` | Remove Wasm build output and retain installed tools. |

The PGlite migration is pinned to commit
`faa505ad2ea0d14dfb1c99ab9614361c39d2c7e2` of `electric-sql/pglite`.
[`cases.json`](../tests/wasm/pglite/cases.json) records each executed case's
source and original test name;
[`upstream.json`](../tests/wasm/pglite/upstream.json) records source hashes and
the inventory of adapted and inapplicable files. The Apache-2.0 license and
attribution are retained alongside the tests. Running them does not need a
PGlite checkout or the PGlite npm package.

The migrated cases cover SQL commands and metadata, typed parameters and
arrays, transactions and error reuse, simple/English full-text search,
PL/pgSQL, row/event triggers, roles, database creation, SQL large objects,
COPY, pgvector, MessageContext reset, payload sizes and close/reopen behavior.
They execute the real PostGamma `Database`/`Connection` API. Expected values
use exact text and OIDs instead of PGlite's JavaScript decoding; transaction
callbacks become explicit SQL, `/dev/blob` COPY becomes streaming COPY, and
trigger delivery uses SQL audit rows. Notification fixtures now use real
`Connection.onNotification()` callbacks and SQL LISTEN/UNLISTEN. `exec()`
submits the complete script through PostgreSQL's simple-query protocol;
regressions cover statement boundaries, implicit rollback, explicit commits,
COPY continuation and cancellation. Parameterized `query()` remains a single
statement. These adaptations do not validate PGlite-specific JavaScript APIs.

The upstream large-result fixtures are adapted to the published PostGamma
limits: collected results above 32 MiB must reject and leave the session usable;
iterator checks transfer at most 16 MiB per case. The 8 KiB payload cases retain
their original data size, and 5 MiB/12 MiB cases require successful full data
roundtrips through exec, literal and parameter queries, plus generated values.
Dedicated cases check exact 16 MiB UTF-8/binary values, rejection above the
value and materialized-result bounds, a full 128 MiB cumulative stream,
cancellation and reuse. The 128 MiB single-value expectation remains outside
the 16 MiB value contract; this is not evidence of GiB-scale capacity.
The MessageContext regression retains all 300
100 KiB inserts and its original 5 MiB bound, and is not a browser VM leak test.
Unsupported extensions, XML, live queries, JS formatting,
custom codecs, startup options, Node/Deno wrappers and IndexedDB are outside
this migration. Linux WebKit coverage does not replace Safari/iOS testing.

Complete database transfer has a separate `make wasm-archive-check` gate in
Chromium, Firefox and Linux WebKit, also included in `wasm-check-full`.
`Database.dumpDataDir()`, `Database.loadDataDir()` and `Database.clone()` use
bounded streams and an exact asset-build identity. Export closes an idle source;
restore requires a new destination and verifies a complete chained-checksum
archive before starting PostgreSQL. The gate covers ephemeral/OPFS transfers,
indexes, sequences, large objects, pgvector, invalid input and cancellation.
See [the package API contract](PACKAGE_README.md) for lifecycle and format limits.

Use `WASM_PGLITE_ENGINES=chromium WASM_PGLITE_STORAGE=ephemeral` with
`wasm-pglite-check` for a targeted run. The full release gate fixes all six
profiles regardless of these overrides. Reports under `build/wasm/pglite/`
contain per-case results, timings, SQL-step progress, browser errors and native
close-resource counts; the latest pointer is `build/wasm/reports/pglite.json`.
`--build-receipt` on `buildsys/wasm_pglite.py` can reuse a matching build for
diagnosis: source and artifact hashes must still match before and after the
run. Each case has a 90-second browser deadline, and each profile has a
20-minute outer bound. Timeouts remain failures and completed results survive
a subsequent browser crash. After an ordinary case stalls, the runner closes
that browser and continues at the next case in a fresh database. The stalled
case stays failed and is never retried. Dependent close/reopen cases cannot be
resumed this way; unfinished cases are listed explicitly. Separate regressions
retain default materialized-result behavior and streaming after a prior
collection-limit cancellation, even when a bounded streaming matrix passes.

The mixed-load gate keeps four connections active against an OPFS database.
Each seeded round combines committed updates, transactional DDL and savepoint
error recovery, streaming COPY commit/rollback, LISTEN/NOTIFY and query
cancellation. Every round checks all durable rows against a host-side model,
checks rolled-back objects are absent and verifies zero outstanding native
requests. Every 50 rounds it alternates clean close, Wasm trap and abort;
recovery must preserve acknowledged commits and discard uncommitted changes.
The SQL sequence is reproducible; native thread interleavings are not fixed.

For final alpha acceptance, use `make wasm-alpha-test` instead of an isolated
soak. It first requires every release stage, including native SQL/COPY oracles,
PGlite cases, vector/contrib, archive/restore, OPFS recovery, lifecycle and the
independently installed three-engine package matrix. Endurance then copies and
rechecks that exact production package, without relinking its kernel or enabling
the private testing bridge. Its recovery cycles alternate orderly close and
control Worker errors/unhandled rejections injected by the test bootstrap.
Native pthread trap/abort recovery remains mandatory in the separate lifecycle
matrix. The receipt records both failure sources explicitly.

All three engines run serially for at least 7200 active seconds each, followed
by cleanup checks; the existing 128 MiB resource budget is unchanged. Source,
package and SDK/kernel asset hashes must remain unchanged through completion.
`build/wasm/reports/alpha.json` references the final receipt and npm archive.
This takes about six hours plus preparation and functional tests. It does not
publish to npm. Short replay and diagnostic reports cannot qualify alpha.

```sh
# One command, including preparation; two hours in each of three browsers.
make wasm-soak-test
# Two hours in Chromium, or an eight-hour overnight run.
make wasm-soak-test WASM_SOAK_ENGINES=chromium WASM_SOAK_SECONDS=7200
make wasm-soak-test WASM_SOAK_ENGINES=chromium WASM_SOAK_SECONDS=28800
# Reproduce a failing seed through a specified round (not duration evidence).
make wasm-soak-test WASM_SOAK_ENGINES=chromium WASM_SOAK_SEED=20261003 WASM_SOAK_ROUNDS=150
```

Reports under `build/wasm/soak/<run>/` retain the seed, parameters, exact asset
hashes, SQL/COPY event log, recovery counts and resource samples. The latest
pointer is `build/wasm/reports/soak.json`. Each run serves its own frozen assets,
so another test rebuilding the shared variant cannot change a reopened module.
Failure snapshots include the active round and Worker wait state. Missing
progress, incomplete duration, SQL/model mismatches and resource-budget failures
fail the command; failed rounds are not silently retried.

On Linux x86-64, the WebKit soak uses the same pinned, unmodified upstream WPE
322662@main as the native Worker diagnostic, including the Worker VM ownership
repair absent from the Playwright bundle. It exchanges bounded, acknowledged
events over a local WebSocket and invokes the existing read-only control/OPFS
diagnostic through a test bootstrap that imports the original verified module.
SDK/kernel assets and manifest hashes remain unchanged. The 128 MiB growth
budget and SQL/recovery checks are unchanged; native Linux Worker threads must
retire within ten seconds. The runner checks and records the browser receipt.
On Linux, the Firefox soak also launches the pinned browser directly, without
Juggler, BiDi, response-body collection or an inspector. It uses the same
acknowledged relay and independently verifies DOM Worker retirement through
`/proc`. Both native drivers require a complete sixty-second natural idle
after final close, within the existing duration-plus-startup/cleanup timeout.
The original SDK/kernel assets remain frozen and checksum-verified. Chromium
keeps its Playwright driver. Other Firefox and WebKit functional
gates still use the separate Playwright bundle; a native soak does not qualify
Safari/iOS or the full release matrix.

For attribution on an already prepared build, run Chromium diagnostics separately:

```sh
make wasm-memory-diagnose WASM_MODE=release
make wasm-memory-diagnose WASM_MODE=release WASM_MEMORY_WORKLOAD=opfs-churn WASM_MEMORY_ITERATIONS=10000
make wasm-memory-diagnose WASM_MODE=release WASM_MEMORY_WORKLOAD=opfs-reuse WASM_MEMORY_ITERATIONS=10000
# Ordinary Worker lifecycle only; no kernel build, inspector or forced GC.
make wasm-memory-diagnose WASM_MEMORY_WORKLOAD=workers WASM_MEMORY_ENGINE=webkit WASM_MEMORY_ITERATIONS=1000
```

Reports under `build/wasm/memory/<run>/` record each browser process, Window,
control/storage heaps, healthy quiescent allocator usage, and each
backend's memory contexts. OPFS-only workloads compare file creation/deletion
with repeated writes to one existing file, without loading Wasm or PostgreSQL.
Allocation profiles and storage heap snapshots are retained beside the report.
Attaching profilers and taking snapshots can change GC and memory use; the
receipt is always diagnostic and cannot qualify the release resource gate.
The original growth gate remains recorded even when a diagnostic continues
beyond its first resource failure.
Parked native pthreads are never entered for JavaScript evaluation. Global
browser memory dumps can be incomplete when those threads cannot service a
dump request; their success flags and the partial trace remain observable.

The `workers` diagnostic launches the native browser directly and creates and
terminates groups of fourteen ordinary module Workers, matching the application
pool size without loading the SDK, Wasm, PostgreSQL or OPFS. It runs three waves
by default (`WASM_MEMORY_WAVES`), waits sixty seconds between waves
(`WASM_MEMORY_IDLE_SECONDS`) and at least 120 seconds after the last wave.
Reports compare the last three natural-idle samples in each wave and retain
process memory, thread counts and JS wrapper finalization counts. Finalization
counts do not measure native VM destruction. Chromium, Firefox and WebKit are
selected with `WASM_MEMORY_ENGINE`. On Linux x86-64, the WebKit diagnostic installs
and uses the unmodified official WPE 322662@main bundle containing the Worker
ownership repair. Its revision, archive size and SHA256 are pinned; all extracted
files are checked against an installation receipt before reuse. Run
`make wasm-webkit-setup` to prepare it separately. `WASM_BROWSER_EXECUTABLE` can
select an explicit comparison build. WebKit's native allocator status is saved
in `browser.native.log`.
The command returns diagnostic completion; it does not substitute for mixed-load
acceptance or automatically declare observed growth acceptable.

The bundled Playwright 1.63.0 WebKit 2359 has a confirmed native Worker leak:
`JSVMClientData` stores an unowned `JSHeapData` pointer, so the heap data and
server IsoSubspaces survive VM destruction. WebKit fixed the ownership and
destruction order in
[6cf979a / 322643@main](https://github.com/WebKit/WebKit/commit/6cf979a79f7bb78c0635ff43f02282d78137583e)
on October 4, 2026. A native-allocation reproduction with 4,200 retired ordinary
Workers retained exactly 4,200 allocations of 7,560 bytes for the generated
subspace array and 4,200 allocations of 2,984 bytes for the heap data.
An isolated ownership backport reduced the second 14,000-Worker wave's natural
idle memory growth from 173.59 MiB to 1.11 MiB with the original native allocator.
That backport is a diagnostic experiment, not a supported SDK/browser repair.
The unmodified official
[WPE nightly 322662@main](https://wpewebkit.org/built-products/x86_64/release/nightly/MiniBrowser/)
contains that source fix and completed the same two 14,000-Worker waves with
1.70 MiB growth and all 28,000 JS wrappers naturally finalized. The pinned
Playwright bundle is unchanged and still affected; the native diagnostic now uses
the pinned official WPE build by default on Linux x86-64. It does not replace the
Playwright executable: the two builds have different automation protocols.
Official WPE and Playwright MiniBrowser profile options are detected
without attaching an inspector.

A second confirmed native leak remains in both that WPE build and current
[WebKit WorkerMessagingProxy.cpp](https://github.com/WebKit/WebKit/blob/9eb4e2ad5983af0d494b62b38e55ff9010fb5993/Source/WebCore/workers/WorkerMessagingProxy.cpp):
`WorkerUserGestureForwarder::create` returns a newly allocated object through
`Ref` without `adoptRef`, leaving its initial reference alive after every
main-thread `Worker.postMessage`, including messages without a user gesture.
The original 1,000-round mixed-load allocation trace retained all 157,588
16-byte allocations at that call site after close and idle. Those tiny objects
also pin native allocator pages. With only the first browser fix, an unchanged
mixed-load run still failed at round 6,650 with 129.61 MiB accounted growth.
The SDK now lets every control and pthread Worker transfer its mailbox to the
API owner before commands start; all subsequent commands and replies use
`MessagePort`. Worker event handlers, transfer lists, direct OPFS ports and
native termination remain intact. The native soak records and requires zero
main-thread native `Worker.postMessage` calls. The original 128 MiB growth and
ten-second thread retirement gates remain; long-run results qualify separately.

On this Linux runner, quiescent samples track browser process-tree RSS/PSS/SwapPss,
threads, file descriptors, Wasm linear memory and OPFS handles. After a
60-second warmup, medians of five samples at the beginning and end are compared against
growth limits: 128 MiB for both RSS and anonymous/shared-memory PSS plus SwapPss
(`WASM_SOAK_GROWTH_MIB`), 32 browser threads, 128 file descriptors and 8 virtual
OPFS opens. Physical OPFS handles remain bounded at 63,
Wasm threads at 13 and linear memory at 512 MiB. Growth comparisons use the same
point before each reopen, after PostgreSQL's file cache has warmed; cold reopen
samples are retained but are not compared to a warmed cache. Normal close must release all
native locks/shared-memory/DSM/pipes and workers; fatal cleanup gates queued
SDK mailbox/cleanup callbacks before terminating their native lock owners and
must release the storage owner before immediate reopen. The parent transport
owns control Worker termination after consuming its cleanup receipt, on both
orderly and fatal paths. A faulting pthread acknowledges retirement from its
own JS event loop and closes its scope; the control Worker waits for that
acknowledgement and records timeouts as cleanup failures. Normal close waits
for native joins and SDK pool return, then terminates every idle pthread before
releasing storage ownership. The pinned SDK clears each thread's own pointer
before publishing native exit. Worker termination counts record requests;
the browser target checks independently verify physical reclamation. Growth limits are
checked throughout the run, so a leak stops the workload early. Node driver
RSS, heap and external buffers are recorded separately from the browser budget.
These are regression budgets, not a
proof of zero leaks; RSS includes shared pages more than once. Linux WebKit
does not establish Safari/iOS behavior.
Total PSS and file PSS are also recorded. File-backed shared code pages are
apportioned across all host processes; another browser exiting can raise this
run's PSS without a new allocation. The allocation trend therefore uses
`Pss_Anon + Pss_Shmem + SwapPss`, while the RSS gate still includes resident
file pages. On older Linux kernels without `smaps_rollup`, the sampler reads
every `smaps` mapping instead. Separate PSS categories remain `null` and their
measurement is explicitly `not-run`; resident `Anonymous` is not substituted
for weighted PSS. The sampler bounds anonymous/shared PSS from resident and
proportional page counts, uses the backing filesystem to identify tmpfs, and
includes rounding uncertainty. Unknown backing stays ambiguous; filenames
never classify copy-on-write pages. The growth gate compares the final median
upper bound against the initial median lower bound, with the same 128 MiB
budget. Thus uncertainty can make a run fail, but cannot make excessive
anonymous/shared growth pass. Reports retain both bounds and identify the
measurement as `smaps-bounds`. Missing required fields or invalid bounds fail
the measurement gate. No host kernel upgrade is required for this fallback.
The API owner creates the control Worker and all 13 pthread Workers as siblings;
private MessagePorts preserve the SDK message interface. This avoids retained
nested Workers during repeated browser teardown. The transport terminates the
entire group after cleanup, including control failures and cleanup timeouts.
Closed transports release their Worker references, callback properties and
MessagePort listeners. Per-instance identities use URL fragments so all
instances keep the same HTTP asset cache keys.
Manifest downloads and downloads for asset integrity verification use the
request policy `cache: 'no-store'`. Firefox can retain temporary response-body
copies with its default request policy, even when the server sends a
`Cache-Control: no-store` response header. Bypassing that cache avoids duplicate
download buffers during repeated opens. SHA-256 and byte-length verification
still apply, and the bounded compiled-module cache still shares immutable code.
The harness counts control and pthread Worker construction and termination independently
of debugger targets. Retired targets must disappear within ten seconds;
final close must leave no control or pthread target. These checks run before the
memory warmup period has completed, catching early retained instances.

`make wasm-test` and `make wasm-check-full` include a 120-second mixed workload
in each browser. This short gate checks correctness and recovery; reports mark
the resource trend as `not-run` if there are fewer than ten complete cycles after warmup. A
duration run of at least 300 seconds requires that resource trend gate to pass.
Run `wasm-soak-test` separately for endurance acceptance.

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
reserves and 264 MiB of estimated allocations, grows shared memory, verifies refreshed
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
kernel validation, or a full browser matrix. `make wasm-check-full` combines the
SQL, vector, storage, package and matrix gates described below. The SQL gate validates its
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
all browser SQL scenarios. Use `WASM_SQL_SCENARIO=abi` to select only
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
Collected queries are limited to 32 MiB including bridge metadata; larger
results use async iteration. A materialized native result has a separate
24 MiB cap: use `chunkRows` when a statement exceeds that cap. Each value
is limited to 16 MiB of encoded data.
Exact text values stay strings, binary values stay owned byte buffers, and
SQL errors expose SQLSTATE plus the native diagnostic fields.

`make wasm-sql-check WASM_SQL_SCENARIO=bindings WASM_MODE=release JOBS=2`
runs twelve binding and request-lifetime regressions adapted from scenarios in
PGlite, DuckDB-Wasm, SQLite Wasm, and chDB-Wasm. They cover arrays and JSONB,
binary subviews, text NUL rejection, empty-result metadata, mixed-type batches,
iterator cleanup, byte-fragmented CSV COPY, active computation cancellation,
and PostgreSQL message-context reclamation. Source references are recorded in
`tests/wasm/sql/bindings.ts`; the tests use postgamma's public API and require no
upstream engine package. This group also runs in the default SQL suite and CI.

The setup follows the [Emscripten SDK instructions](https://emscripten.org/docs/getting_started/downloads.html),
and the harness uses the [Playwright library](https://playwright.dev/docs/library).

`WASM_SQL_SCENARIO=lifecycle` exercises close during lock waits, result/COPY
backpressure and handle submission. `fatal-trap` and `fatal-abort` exercise real
test-only faults and fresh-module recreation. Normal close waits for native
resource and pthread reclamation. Fatal teardown uses a separate MessagePort
to quiesce the I/O owner's native callbacks before terminating other pthreads,
then drains outstanding browser I/O and closes physical OPFS handles without
entering the failed Wasm module. Only its
close acknowledgement permits an affirmative `storageReleased` diagnostic;
cleanup errors and timeouts remain visible. Ownership release and host request
rejection follow that cleanup attempt. The faulting pthread independently
acknowledges retirement through a private channel and closes its own scope.
Its parent waits for that reply before completing fatal cleanup; it forces
termination on timeout and reports `crashedWorkerRetired: false`. The browser still does not provide a
synchronous VM termination acknowledgement. Abrupt Worker and browser crash
tests bypass this cleanup path and independently exercise WAL recovery.

Connection disconnect waits for backend reference release in bounded steps on
the existing host driver. While that connection is closing, other connections
can consume results, execute SQL and cancel requests. Retries share a 10-second
deadline; timeout retains the connection for another close attempt. New work on
that partially closed connection is rejected. A 1 ms readiness retry is armed
only while backend release is pending; idle operation remains notification-driven.
The `lifecycle` scenario holds a real backend endpoint reference to verify other
connections keep progressing. `disconnect-timeout`, included in the full SQL
gate, checks the deadline, retry and stale-generation behavior.

Cancellation, request release and COPY close also yield between bounded C
steps. Pending cancellation tickets retain their request payload until the
supervisor acknowledges completion. Release discards at most one result or
8 KiB COPY fragment per step; timeout retains the handle for retry. Connection
startup uses CONNECT begin/poll with a reserved connection and carrier slot.
Startup output is staged within 64 KiB and transferred in bounded pieces;
failure or deadline expiry retires the ticket and backend before releasing
the reservation. If that retirement cannot finish, the API starts instance
discard after 90 seconds, covering the native 60-second admission and 30-second
handshake budgets. It stops new commands and waits for fatal cleanup before
rejecting callers. Existing connections become unusable; a replacement requires
opening a new instance. In-flight native ticket payloads are retained until the
whole instance is retired. The private byte protocol is version 7.

The three-engine lifecycle matrix includes `control-progress`,
`control-timeout` and `connect-progress`: cross-connection SQL/cancel,
10-second release timeout/retry, startup backpressure with a 32-byte queue,
failed and expired startup retirement, and two-worker admission with a pinned
transaction. A held-ticket fixture also checks the 90-second instance discard,
OPFS release and immediate recovery of a committed row. Its browser envelope is
180 seconds; the production startup deadline remains 90 seconds. Test-only
ticket gates are excluded from production builds.
Reused receipts include the native C test fixture identities as well as the
kernel, bridge, TypeScript harness and generated assets.

Browser failures retain a control-Worker snapshot before context teardown:
active command metadata, native driver phase, live pthread wait addresses,
and the OPFS owner's pending operations and handle counts. Inspection uses
atomic reads and a separate owner port; it submits no SQL and does not wake
the native driver. An unresponsive target has its own diagnostic timeout.
The `diagnostics` lifecycle fixture holds the driver on its notification word,
checks two snapshots without releasing it, then explicitly resumes SQL. It
runs in all three engines through `make wasm-check-full`. This validates
observability; it does not establish the cause of the earlier intermittent
WebKit reopen stall.

The accepted SQL limits are generated for both C and JavaScript from
`manifests/profiles/wasm-resource-budget.json`: four live connections, 24 MiB
native result buffers, 16 MiB values, 16 KiB COPY fragments, 32 MiB pending
query bytes per connection, and 64 MiB queued Worker messages. Commands are
limited to 32 MiB. Admission counts actual UTF-8 bytes without allocating an
encoded copy. `budget` checks
all four carriers and admission/queue limits. The full run writes
`sql-resource-budget.json` beside its `sql.json`; the latter includes all
scenario results and source/asset identities. Diagnostics sample on commands
and lifecycle transitions by default. To sample during long commands or initdb,
pass `diagnostics: {sampleIntervalMs: 10}` to `Database.open()`; `0` disables
periodic sampling. Sampling can miss transient peaks; the 13-slot SDK pool
supplies the hard pthread bound. Caller-retained results
and the browser's VM/code memory are outside managed queue accounting.
The `capacity` scenario additionally checks four simultaneous 5 MiB
roundtrips and a paused large-result consumer while another connection runs.
The linear-memory maximum remains 512 MiB; these limits are separate bounds,
not a promise that every connection can saturate every bound simultaneously.

The host driver sleeps on notifications between commands. Replies and
per-connection result/COPY readiness use events. Pending lifecycle steps use
a 1 ms retry for supervisor/backend states without a public waitable; idle
operation has no periodic readiness timer. The lifecycle gate records
idle driver wakeups separately from browser CPU or power consumption.

`connection.exec(sql, {chunkRows})` collects a multi-statement script;
`executeScript()` returns its streaming request, including COPY streams.
The server parses SQL and owns transaction boundaries: an error rolls back
the implicit script transaction and skips subsequent statements, while an
explicit earlier COMMIT remains committed. Scripts accept text results and
no parameters; use `query()`/`execute()` for typed parameters or binary rows.

Register `connection.onNotification(({channel, payload, pid}) => { ... },
onError)` and execute SQL LISTEN on that connection. The returned function
removes the callback; SQL UNLISTEN changes the server subscription. An idle
listener releases its carrier and resumes on a notification signal.
Notifications arrive without SQL polling;
callbacks follow PostgreSQL commit/rollback and channel-quoting semantics.
The native 64-event queue and 16-event drain batches bound pending delivery.
Overflow calls `onError` with `NotificationOverflowError.dropped` (a bigint);
without an error handler it is reported to the console. Throwing or rejecting
callbacks are isolated from SQL, and closing a connection retires its callbacks.

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
I/O thread, at most 63 cached data access handles plus two metadata handles, and
stable file identities behind a checksummed namespace journal. Checkpoints
alternate between two 8 MiB slots: the new snapshot is flushed before the old
slot is retired, and no later mutation is acknowledged until retirement finishes.
Snapshots retain live file IDs and the next-ID watermark; replay resets only the
per-generation record sequence. The two slots fit within 16 MiB and the namespace
allows 16,384 live entries. Migrating the previous append-only format temporarily
needs room for one additional snapshot of at most 5 MiB; the previous log remains
authoritative until it is retired. Older clients reject the upgraded format.
The profile does not provide symlinks
or external tablespaces. It requires OPFS and Web Locks and never falls back to
an ephemeral database when persistent storage cannot be opened.

Wasm cluster creation has a 120-second native startup budget because OPFS
initialization creates and synchronizes more than a thousand files. Opening an
existing cluster with `create: false` retains its 60-second budget. Connection
startup retains its 60-second admission and 30-second handshake budgets.
Cross-browser lifecycle and PGlite startup fixtures allow
180 seconds for cold loading, creation and cleanup; SQL case deadlines remain
90 seconds and fatal cleanup plus immediate recovery must still finish within
30 seconds. Subsequent fatal iterations retain their 90-second fixture deadline.
File lengths are cached under WasmFS file locks; a failed write or truncate
invalidates the cache.
Repeated fsync skips browser I/O only after this owner has successfully flushed
the same file and no intervening mutation occurred. Recovered files begin with
unknown durability and require a real flush.

Chromium storage cases include cold target initdb and reopen with a separate
90-second case deadline and record the observed duration. The durability case keeps fsync
and synchronous commit enabled, pauses actual WAL flush before COMMIT acknowledgement
and lock release, and checks unrelated session progress. Short writes, quota,
file/directory sync and teardown failures propagate to callers. A missing namespace
journal rejects opening a nonempty store and preserves its data files.

The storage-errors case verifies that a WAL flush failure cannot acknowledge
COMMIT and that uncertain namespace writes require a fresh module. Recovery must
retain acknowledged commits and transaction atomicity; a transaction whose reply
was lost may already be committed. The OPFS resource checks include the measured
2 MiB I/O stack, existing 13-Worker pool, 512 MiB linear memory maximum, bounded
queues, 63 data access handles plus two metadata handles, and healthy shutdown drain.
Browser VM/RSS and hardware power-loss durability are not measured.

The recovery group uses one origin and persistent browser profile. Each of
Worker termination and SIGKILL of the harness-owned browser process group is
injected before/after WAL flush, during checkpoint data writes, file creation,
relation extension, before/after namespace rename flush, and after flushing a
partial namespace record. Both also interrupt target initdb. The runner verifies
PostgreSQL automatic recovery, acknowledged transaction pairs, DDL/extension
rollback, file identity and actual journal-tail replay. Seven checkpoint boundaries
also interrupt snapshot preparation, partial and complete writes, flush, retirement
and publication. Its complete 32-case
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

`make wasm-contrib-check JOBS=2` exercises pg_trgm 1.6 and hstore 1.8 in
Chromium, Firefox and Linux WebKit. Their sources and SQL resources come from
the pinned PostgreSQL checkout. The target compilation inventory must match
the reviewed state policy: pg_trgm's three GUC backing variables and lazy
comparison function pointer belong to each logical backend; hstore currently
has no mutable static candidates. New state fails the build until reviewed.
The gate checks extension DDL rollback, real GIN/GiST plans, four concurrent
connections with independent settings, connection reuse, OPFS reopen and
archive roundtrips. The pinned PGlite GIN, GiST and hstore cases also run in all
six PGlite browser/storage profiles.

The package report names the staged directory and npm tarball under the selected
variant's `package/<build-id>/` and `dist/`. Install that tarball with
`npm install /path/to/postgamma-wasm-0.1.0-alpha.1.tgz`. The package defaults to
`WASM_PACKAGE_EXTENSIONS=standard` (pgvector, pg_trgm and hstore).
`WASM_PACKAGE_EXTENSIONS=none` selects the extension-free build; `pgvector` or
a comma-separated subset selects only those modules.
No command publishes to a registry.

The independent consumer installs with lifecycle scripts disabled, resolves
the package exports and generated declarations, and serves only its own files.
It checks parameters, transactions, four actual concurrent sessions, cancellation,
HNSW/IVFFlat, contrib functions, archive/clone, OPFS reopen and an explicitly relocated asset directory. Negative
cases reject mismatched build/protocol identities, corrupt Wasm/resource bytes,
a missing Worker and missing cross-origin isolation before reporting success.

`postgamma.assets.json` and generated loader identity bind the API to its private
bridge and exact runtime assets. The loader verifies core asset hashes before
instantiation; the package gate checks every declared asset, notices, version and
absence of checkout paths. TypeScript is a pinned development dependency only.
Follow the [consumer guide](PACKAGE_README.md) to copy the complete asset tree,
set HTTP headers and use `assetBase` with a bundler. Runtime browser support and
performance must be reported for the actual tested profile.

## Storage and SDK adapter contracts

`manifests/wasm-storage-protocol.json` defines storage operation numbers and
Emscripten errno mappings. Its C enum and TypeScript constants are checked in
and verified by `doctor-wasm` and kernel linking. After an intentional contract
change, regenerate both with
`python3 buildsys/wasm_storage_protocol.py --root . --write`. C compilation also
checks the error numbers against the target libc. Requests use named fields;
JavaScript receives scalars and byte views, never native structure offsets.

`manifests/profiles/wasm-sdk-contract.json` records the reviewed mailbox/proxy
source hashes for SDK 6.0.9. The adapter uses the SDK's postMessage path with
`waiting_async` clear, its C proxy queue, and the C++ queue handle needed for
direct I/O delivery. Doctor and kernel linking reject a changed SDK contract
before compiling kernel objects. An SDK upgrade requires reviewing these
sources and rerunning platform/runtime probes, blocked-control-worker I/O,
close/cancellation/reuse, storage recovery and all six package profiles before
updating the recorded contract. The source SDK itself is left intact.

## Continuous validation

After `make source-init wasm-setup`, `make wasm-ci JOBS=2` runs the same
debug Chromium gates and build-system tests used for pull requests by
`.github/workflows/wasm.yml`. Relevant pushes to `main` or `feature/wasm`, and
manual dispatch, run `make wasm-release JOBS=2`, including all browser engines.
The workflow has no scheduled trigger. Changes to shared runtime, compiler,
C API and common build inputs also select a separate native fast-regression job.

Every CI run includes `wasm-runtime-check`. Pull requests that change platform,
SDK, bridge or shared kernel inputs also run `wasm-platform-check`,
`wasm-wait-check` and `wasm-recovery-check`. Use
`make wasm-ci WASM_PLATFORM_CHECKS=1 JOBS=2` to include these probes locally.
The full and weekly suites always include all four gates, before SQL and
package checks; a failed probe stops the suite. Release and weekly acceptance
also run the three-engine OPFS lifecycle matrix, with 20 trap/reopen and 20
abort/reopen cycles per engine, before accepting a consumer package. Reusing a
SQL receipt requires unchanged kernel sources, bridge, and browser assets.

The Wasm job uses Ubuntu 24.04, matching LLVM/Clang 22 host AST tools and the
pinned SDK/browser package. It retains stage logs, source/tool identities and
failure reports separately from native evidence; successful runs also retain
the tested npm archive. Browser profiles and object/generated trees are not
uploaded. Each job has a 180-minute bound and artifacts expire after 14
days. A local pass or workflow syntax check does not establish a hosted CI pass.

For the heavier suite, install all browser engines once and use:

```sh
make wasm-browser-setup
make wasm-check-full JOBS=2
make wasm-weekly JOBS=2
```

`wasm-check-full` validates the pinned PostgreSQL source. `wasm-weekly` resolves
the current PostgreSQL master revision and runs that same suite in an isolated
project/source/build tree under `build/wasm/upstream/`. It leaves the pinned
checkout and manifest in place. Reports retain the exact revision, current
tracked project inputs, each stage's elapsed time/free space and failure logs.
The native SQL oracle has its own build directory for each candidate. A failed
candidate reports the failing gate; it does not update ownership decisions or
declare compatibility from native tests alone.

Select a branch, mirror or exact previously reported revision with:

```sh
make wasm-weekly PG_BRANCH=REL_19_STABLE PG_REPOSITORY=https://github.com/postgres/postgres.git JOBS=2
make wasm-upstream-canary PG_REF=<commit> CANARY_PROFILE=weekly JOBS=2
```

These commands reuse installed SDK/browser tools. Candidate source and build
trees remain for diagnosis, so repeated candidates need additional disk space.
The full suite includes ABI/pthread/TLS/libc, wait/notify, longjmp and shared
runtime probes, all SQL groups, vector queries and index plans,
OPFS ownership/failure/recovery checks, an independently installed package, and
Chromium/Firefox/Linux WebKit package checks and measurements. Every engine must
pass both ephemeral and OPFS profiles. OPFS uses normal persistent browser
contexts; private browsing can disable the storage API. The full storage fault
and crash-recovery suite is validated in Chromium.
`make wasm-release JOBS=2` repeats the full suite with `-O2` and records the
actual build mode. Its pgvector comparison requires a passing base SQL budget
from that same mode and source. Actual Safari and mobile-device acceptance
requires runs on those devices; Linux WebKit does not provide that evidence.

The PostgreSQL 20 master candidate `7a0aa99e51c7a3263e6ee45f19f270ddae430a92`
passed the full debug suite on 2026-09-13, including all six engine/storage
package profiles. Target source adaptations record their input/output hashes;
unknown source shapes and unreviewed state still fail generation. Candidate
validation leaves the product pinned to PostgreSQL 19beta3 and does not declare
all later master commits compatible.

The existing **PostgreSQL upstream canary** workflow runs every Sunday at 02:17
UTC and supports **Run workflow** with `weekly` or shorter `daily` profiles.
Wasm has separate pinned/REL_19_STABLE/master jobs, each limited to 360 minutes
and two compilation jobs, with diagnostics retained for 14 days. Scheduled jobs
skip only when the same project/PostgreSQL/pgvector/profile inputs already
passed that Wasm suite; changed upstream commits and failed prior runs are
tested again. Manual dispatch always runs. Native and Wasm pass histories are
separate. There is no additional daily schedule or email integration.

A local pinned full-suite run on 2026-09-12 took 1,749.5 seconds (29.2 minutes)
with `JOBS=2`, cached capture/native-oracle inputs and rebuilt Wasm core/pgvector
objects. Installed tools occupied about 3.2 GiB; the target tree with retained
run history occupied 1.5 GiB, plus a 167 MiB native oracle. Candidate source and
build trees need additional space. These observations are not a cold hosted
runner budget; each hosted run retains its own timing/space evidence.
