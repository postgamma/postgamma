# Browser preview

The browser package runs the PostgreSQL kernel in Workers, with four independent
SQL sessions and static pgvector, pg_trgm and hstore. Debug and optimized release
wasm32 packages with pthreads have passed in Chromium, Firefox and Linux WebKit. This package is
a source-built preview; an npm registry release has not been published.
See the [browser preview release notes](../releases/wasm-0.1.0-alpha.1.md)
for the accepted scope, data compatibility and release gates.

## Try it on the homepage

The [homepage](../index.md#live-demo) starts a real upstream **psql** compiled
alongside the WebAssembly kernel. It starts automatically when the page loads;
once `postgres=#` appears, type directly into the terminal and press Enter:

```sql
CREATE DATABASE test;
\c test
CREATE TABLE t1(c1 integer, c2 text);
INSERT INTO t1 VALUES (100, 'hello');
SELECT * FROM t1;
\d t1
\?
```

While it starts, the compact terminal shows a centered loading indicator with an
estimated percentage and elapsed time. The estimate follows startup stages;
it is not a download byte counter. Use the expand button for a larger workspace.

psql handles SQL parsing, prompts, backslash commands and result formatting.
The page provides an xterm.js terminal with inline input and output. Enter sends
a line; psql supplies the continuation prompt for unfinished SQL. The arrow keys
edit the current line or recall history; Home/End and Ctrl+A/E move within it.
Paste accepts multiple lines, including `COPY FROM STDIN` data. Ctrl+C interrupts
a query or clears unfinished input; Ctrl+L clears the screen. `\q` or Ctrl+D
exits; Enter starts a fresh session after exit. **Close** releases the instance
and **Reset** creates a fresh memory database. The [SQL editor](../index.md?client=js#live-demo) retains
the aggregation, JSONB, transaction and pgvector examples.

Use HTTPS or localhost to run the demo. GitHub Pages needs one automatic reload
on first start to enable the browser's isolation requirements. The preview does
not save data to OPFS. Native terminal input/output queues are bounded to 64 KiB
each, each input line is at most 16 KiB, pending input is capped at 64 KiB, and
terminal scrollback is capped at 2,000 lines. Large results continue streaming
while old displayed output is removed. Terminal assets are bundled with the site;
no terminal CDN or backend service is needed.

This first psql integration is experimental and owns its database instance.
It uses the matching private libpq and PostgreSQL wire protocol over the existing
in-memory transport, all inside browser Workers. There is no remote SQL server,
WebSocket proxy, WebContainer or external shell. Shell/editor/pager commands and
network connection strings and signal-timer-based `\watch` are unavailable; readline completion and local file
upload/download are not implemented. Restarting psql creates a new instance.
The SDK's SQL connections, logical backup and data-directory archive operations
cannot run concurrently with a psql terminal in the same instance.

The same static assets can be hosted under a GitHub Pages repository path. The
service worker supplies isolation headers for the demo page and its assets;
all Worker/module/asset URLs are relative to that path. The one-command browser
check is `make docs-live-check DOCS_WASM_PACKAGE=auto`. It exercises real psql
and the SQL editor in Chromium, Firefox and WebKit on both a header-based host
and a headerless `/project/` host. This emulates Pages; it does not publish a site.

For a local checkout, `make docs-serve DOCS_WASM_PACKAGE=/path/to/package.tgz`
bundles a source-built production package. Later `make docs-serve` invocations
reuse it. The preview server supplies isolation headers. When it runs remotely,
run `make docs-serve DOCS_DEV_ADDR=0.0.0.0:8000` and open
`https://SERVER_IP:8000/`. Non-loopback binds automatically enable HTTPS and
generate a reusable self-signed development certificate. Copy
`build/docs/tls/preview.crt` to the computer running the browser and import it
into its trusted certificate authorities. Merely skipping the certificate
warning disables Chromium's HTTP cache and can make Worker startup time out on
slow connections. To supply a certificate already trusted by your browser,
set `DOCS_TLS_CERT=/path/to/fullchain.pem` and
`DOCS_TLS_KEY=/path/to/key.pem`. The default loopback address uses HTTP.

## Build and install

From a source checkout with the native build prerequisites installed:

```sh
make wasm-test
```

This initializes the source and pinned Wasm tools, uses a serial release build
by default, and runs the full Wasm gates including the PGlite-derived tests in
three browsers with ephemeral and OPFS storage. Use `JOBS=2` to increase build
parallelism. Independent tests continue after failures; the final exit status
stays nonzero if any gate fails or cannot run. No package is published.

The summary pointer is `build/wasm/reports/ci.json`; its `run_report` names
the full-suite report under `build/wasm/ci/`. Its sibling `package.log`
records the exact `postgamma-wasm-*.tgz` path. Install that file in your application:

```sh
npm install /path/to/postgamma-wasm-0.1.0-alpha.1.tgz
```

Only building the package requires the SDK, Python and compiler. The installed
package includes JS, TypeScript declarations, Worker modules, Wasm and database
initialization resources, with no runtime dependencies on Arrow or other npm
packages.

## Serve the assets

Copy the complete `node_modules/@postgamma/wasm/` directory to your application's
public directory, for example `public/postgamma/`. Preserve all relative paths,
including `src/`, `postgamma.mjs`, `postgamma.wasm`, `postgamma.data`, and
`postgamma.assets.json`. A bundler must retain this directory in its output.
The imported API and copied assets must come from the same package build.

Serve over HTTPS or localhost. The application page needs:

```text
Cross-Origin-Opener-Policy: same-origin
Cross-Origin-Embedder-Policy: require-corp
```

Use a JavaScript MIME type for `.js` and `.mjs`, and `application/wasm` for
`.wasm`. Host module Workers on the application's origin. Cross-origin page
resources must also satisfy the embedder policy. The loader reports missing
isolation, downloads, or mismatched core assets before opening a database.

With a bundler, import the package normally. For an unbundled page, add this
import map before the module script:

```html
<script type="importmap">
{"imports":{"@postgamma/wasm":"/postgamma/src/index.js"}}
</script>
```

## Query and close

```js
import {Database} from '@postgamma/wasm';

const db = await Database.open({
  assetBase: new URL('/postgamma/', location.href),
  storage: {type: 'opfs', name: 'documents'},
});
const connection = await db.connect();
try {
  const results = await connection.query('SELECT $1::int8 AS value', {
    parameters: [{typeOid: 20, value: '9223372036854775807'}],
  });
  console.log(results[0].rows);
} finally {
  await connection.close();
  await db.close();
}
```

Omit `storage` for an ephemeral database. Closing it discards its data; opening
another ephemeral database creates a fresh cluster. OPFS data belongs to the
origin and database name. Reopen it with the same name and `create: false` to
require an existing database. One origin/name has one live owner, enforced with
Web Locks and a synchronous file access handle. Browser site-data deletion or
origin changes make the old database unavailable. Native PostgreSQL cluster
directories cannot be mounted into this target.

Opening an occupied OPFS database waits up to 5 seconds for the previous owner
to release it. Set `lockTimeoutMs` to change that bound, or `lock: 'fail'` to
reject immediately. A zero timeout also makes one immediate attempt. The bound
applies to acquiring ownership, not the subsequent kernel startup. A waiting
open creates only the control Worker; it allocates no Wasm memory or pthreads
until it owns the database. Pass `signal` to cancel an opening operation; after
`open()` resolves, the signal no longer controls that database.

```typescript
import {Database, DatabaseLockedError} from '@postgamma/wasm';

try {
  const db = await Database.open({
    storage: {type: 'opfs', name: 'documents'},
    lockTimeoutMs: 5000,
    signal: new AbortController().signal,
  });
  await db.close();
} catch (error) {
  if (error instanceof DatabaseLockedError) {
    console.log(error.databaseName, error.reason); // 'busy' or 'timeout'
  } else throw error;
}
```

Tabs do not share an instance or its SQL sessions. Another tab must wait for
the current owner to close; it never steals ownership. Close the database
before application-controlled navigation when possible. Browser shutdown and
navigation release resources asynchronously, which is why reopening waits by
default. History caching varies by browser: a page retained in bfcache can keep
its owner alive. Do not assume that `pagehide` or pressing Back proves ownership
has been released. The cross-tab gate records whether a real bfcache restore
occurred; navigation without a cached restore is narrower evidence.

Synchronous COMMIT waits for the browser storage barrier. The Chromium recovery
suite covers interrupted Workers/browser processes and storage failures; it
does not establish hardware power-loss durability. Handle quota and I/O errors
and reopen a discarded instance before further work.
`make wasm-multitab-check WASM_MODE=release` additionally checks real tabs,
waiting/cancellation, refresh, navigation, and tab termination during WAL flush
in Chromium, Firefox and Linux WebKit. It runs as part of `make wasm-test`.

Metadata checkpoints reclaim the history of file creation, deletion and rename.
The profile supports 16,384 live file/directory entries and keeps its two
metadata files within 16 MiB in normal operation. Existing preview databases
upgrade automatically when opened; that migration needs up to 5 MiB of extra
space for a snapshot. Older builds cannot open the upgraded storage format.

For a complete same-build copy, use `dumpDataDir()` with `Database.loadDataDir()`,
or `clone()`. Export closes an idle source database; close its connections and
requests first. Restore requires a new target and the exact matching asset
build. Failed OPFS restores retain an incomplete marker and cannot be opened;
use a new name for another attempt.

Each connection serializes `query()` calls; different connections can execute
concurrently. Use `execute()` and its request iterator for large results,
`cancel()` for cancellation, and COPY streams for bulk data. Values preserve
exact int64/numeric strings, NULL and binary bytes. Errors retain SQLSTATE.

Connection startup starts instance discard at its 90-second deadline if it
cannot complete or retire. Callers are rejected after the cleanup attempt, and
all connections on that instance become unusable. Open a replacement before
issuing further work. Cleanup failures remain visible in errors and diagnostics.

Run `CREATE EXTENSION vector`, `CREATE EXTENSION pg_trgm` or
`CREATE EXTENSION hstore` once per database to enable the included modules. The
`examples/wasm/vector-search.html` is available from the
source checkout with `make wasm-vector-serve`.

### Parallel queries

A connection can opt into PostgreSQL parallel query execution:

```js
await connection.exec(`
  SET max_parallel_workers = 2;
  SET max_parallel_workers_per_gather = 2;
`);
```

The supported profile is four independent sessions sharing at most two parallel
workers, in addition to each query's leader. Configure every participating
connection this way. New connections start with parallel workers disabled.
The planner chooses parallel execution when its costs justify it; inspect
`EXPLAIN (ANALYZE, FORMAT JSON)` for `Workers Launched`, since a parallel plan
can fall back to its leader when both workers are occupied. The worker limit is
shared with parallel index maintenance; larger worker counts are outside this
profile. Keep `parallel_leader_participation` at its default of `on`.

`make wasm-parallel-check WASM_MODE=release` exercises parallel scans, partial
aggregates, parallel hash joins and Gather Merge in Chromium, Firefox and
Linux WebKit with ephemeral and OPFS storage. Every accepted parallel plan must
show `Workers Planned: 2` and `Workers Launched: 2` in `EXPLAIN ANALYZE`, including
with leader participation disabled and after OPFS reopen. It compares complete ordered
results with serial execution and checks snapshots, session settings,
cancellation, statement timeout, worker errors, worker-budget fallback,
persistent reopen and resource release. `make wasm-test` includes this gate;
its standalone report pointer is `build/wasm/reports/parallel.json`.

## Measured scope

`make wasm-browser-setup wasm-matrix-check WASM_MODE=release` reuses the pinned browsers
and measures the previously validated package. Reports are under
`build/wasm/matrix/`; each engine runs SQL, four concurrent sessions,
transactions, cancellation, vector indexes and separate ephemeral/OPFS reopen
cases. All three engines are required gates. OPFS is tested with normal
persistent browser profiles; private browsing can disable its storage APIs.
The complete storage error and crash-recovery suite remains a Chromium gate.

The shared linear-memory ceiling is 512 MiB with a fixed 14-slot pthread pool.
JS heap, compilation memory and browser VM/RSS are additional and are not
measured by that ceiling. The timings below are local release observations, not
performance guarantees. Safari on macOS/iOS, mobile browsers, Node.js and WASI
execution have not been accepted. Multiple live instances and
arbitrary native extension loading are outside the initial profile.

Long mixed-load runs currently exceed the 128 MiB browser memory-growth budget.
Functional regression passes do not complete this resource acceptance. Use
`make wasm-soak-test` for the two-hour gate in each of the three engines; release
remains blocked until that gate passes with the original limits.

To validate an optimized archive, run `make wasm-release JOBS=2` after toolchain
setup. This runs the same complete SQL, vector, recovery and browser gates with
`-O2`; reports and artifacts identify their build mode. OPFS requires Web Locks,
BroadcastChannel and synchronous file access handles in addition to the page's
isolation headers.

Measured on Linux x86-64 on 2026-09-13, with Playwright 1.63.0 and the
`0.1.0-alpha.1` release package (`-O2`) built from PostgreSQL 19beta3 and pgvector 0.8.6:

| Engine | Ephemeral package profile | OPFS package profile |
| --- | --- | --- |
| Chromium 153.0.8010.12 | Passed | Passed |
| Firefox 155.0 | Passed | Passed |
| Linux WebKit 26.6 | Passed | Passed |

The npm archive was 3,592,432 bytes (3.43 MiB), with 12,187,668 unpacked bytes
(11.62 MiB), including notices and declarations. The representative query was
`SELECT sum(i) FROM generate_series(1,10000) i`, sampled 20 times after startup.

| Engine / storage | Create | Reopen | Query median / p95 | Peak shared linear memory |
| --- | --- | --- | --- | --- |
| Chromium / ephemeral | 3.64 s | 3.49 s | 21.53 / 32.93 ms | 184.38 MiB |
| Chromium / OPFS | 14.25 s | 1.25 s | 18.97 / 21.05 ms | 128.00 MiB |
| Firefox / ephemeral | 3.66 s | 3.57 s | 23.02 / 34.32 ms | 184.38 MiB |
| Firefox / OPFS | 36.82 s | 1.95 s | 21.85 / 27.40 ms | 128.00 MiB |
| Linux WebKit / ephemeral | 3.57 s | 3.83 s | 24.11 / 31.28 ms | 184.38 MiB |
| Linux WebKit / OPFS | 11.66 s | 1.42 s | 19.96 / 22.90 ms | 128.00 MiB |

Ephemeral reopen initializes another fresh cluster; OPFS reopen opens existing
files. Peaks cover the full SQL/vector workload and are not idle memory sizes.
These are single local runs without exclusive host access; browser caches,
host load and engine scheduling affect timings. The complete record is
`build/wasm/matrix/20260913T033050Z-e3cb95ed/report.json`, including package and
harness hashes, raw samples and version identities. Earlier failed rounds remain
in their own report directories.
