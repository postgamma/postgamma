# PostGamma WebAssembly probes

The current commands validate the Wasm toolchain, ABI, pthread/TLS behavior,
stacks, basic libc interfaces, waits, deadlines, cancellation, and error recovery in Chromium.
They do not build a PostgreSQL kernel or provide a browser SQL API.
The package is private and contains development dependencies only.

From the repository root:

```sh
make wasm-setup
make wasm-probe-check
make wasm-platform-check
make wasm-wait-check
make wasm-recovery-check
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
use bytewise `strcoll`. The probes validate these behaviors explicitly. Full
PostgreSQL locale support, kernel wait/cancel behavior, ERROR recovery, initdb, SQL, and
persistence require later checks.

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

Each build uses
`build/wasm/wasm32-emscripten-pthreads/<mode>/<config-id>/`. Configuration changes
select a separate directory; unchanged probe inputs and verified outputs are
reused. Each attempt retains `runs/<run-id>/report.json` and logs.
`reports/smoke.json`, `reports/platform.json`, `reports/wait.json`, or
`reports/recovery.json` points to the latest attempt,
including failures. Partial browser events are retained when a check fails.
`inputs.json` records source revisions, modified inputs, compiler identity,
flags, and the npm lockfile. Reports distinguish a compiled probe from a
browser pass and always set `kernel_validated` to false.

Browser profiles stay under each run. Chromium's disposable socket directory
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
make wasm-probe-build WASM_PROBE=platform
make wasm-setup WASM_DEPS_DIR=/opt/postgamma-wasm-tools
make wasm-probe-check WASM_DEPS_DIR=/opt/postgamma-wasm-tools
make wasm-probe-check WASM_EMSDK_DIR=/opt/emsdk
```

`JOBS` bounds SDK compilation parallelism, as it does for native builds.

`WASM_EMSDK_DIR` selects an already activated SDK matching the profile's
Emscripten version; setup does not install or activate that SDK. `WASM_NODE` can select a
compatible Node executable, and `WASM_BROWSER_EXECUTABLE` an explicit Chromium
executable. The runner and actual browser version are recorded. The default
uses the managed tools without sourcing `emsdk_env.sh` or changing your shell.

`BUILD_DIR` controls disposable output. Dependency and SDK paths inside it
are rejected, including symlinked paths. Both `make wasm-clean` and the root
`make clean` retain `.deps/wasm/` and `wasm/node_modules/`. Cleaning build output
therefore does not require downloading the toolchain again.

The kernel/package commands `make wasm`, `make wasm-check`,
`make wasm-check-full`, and `make wasm-serve` fail explicitly because they
are not implemented yet. The candidate SDK and probe
settings do not select the final kernel memory, longjmp, or filesystem policy.

The setup follows the [Emscripten SDK instructions](https://emscripten.org/docs/getting_started/downloads.html),
and the harness uses the [Playwright library](https://playwright.dev/docs/library).
