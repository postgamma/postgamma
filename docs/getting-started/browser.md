# Browser preview

The browser package runs the PostgreSQL kernel in Workers, with four independent
SQL sessions and static pgvector. Debug and optimized release wasm32 packages
with pthreads have passed in Chromium, Firefox and Linux WebKit. This package is
a source-built preview; an npm registry release has not been published.

## Build and install

From a source checkout with the native build prerequisites installed:

```sh
make source-init
make wasm-release JOBS=2
```

The full-suite report is under `build/wasm/ci/`. Its sibling `package.log`
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

Synchronous COMMIT waits for the browser storage barrier. The Chromium recovery
suite covers interrupted Workers/browser processes and storage failures; it
does not establish hardware power-loss durability. Handle quota and I/O errors
and reopen a discarded instance before further work.

Metadata checkpoints reclaim the history of file creation, deletion and rename.
The profile supports 16,384 live file/directory entries and keeps its two
metadata files within 16 MiB in normal operation. Existing preview databases
upgrade automatically when opened; that migration needs up to 5 MiB of extra
space for a snapshot. Older builds cannot open the upgraded storage format.

Each connection serializes `query()` calls; different connections can execute
concurrently. Use `execute()` and its request iterator for large results,
`cancel()` for cancellation, and COPY streams for bulk data. Values preserve
exact int64/numeric strings, NULL and binary bytes. Errors retain SQLSTATE.

Run `CREATE EXTENSION vector` once per database for pgvector. The
`examples/wasm/vector-search.html` is available from the
source checkout with `make wasm-vector-serve`.

## Measured scope

`make wasm-browser-setup wasm-matrix-check WASM_MODE=release` reuses the pinned browsers
and measures the previously validated package. Reports are under
`build/wasm/matrix/`; each engine runs SQL, four concurrent sessions,
transactions, cancellation, vector indexes and separate ephemeral/OPFS reopen
cases. All three engines are required gates. OPFS is tested with normal
persistent browser profiles; private browsing can disable its storage APIs.
The complete storage error and crash-recovery suite remains a Chromium gate.

The shared linear-memory ceiling is 512 MiB with a fixed 13-slot pthread pool.
JS heap, compilation memory and browser VM/RSS are additional and are not
measured by that ceiling. The timings below are local release observations, not
performance guarantees. Safari on macOS/iOS, mobile browsers, Node.js and WASI
execution have not been accepted. Multiple live instances and
arbitrary native extension loading are outside the initial profile.

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
