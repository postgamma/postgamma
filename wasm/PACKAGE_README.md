# postgamma for the browser

PostgreSQL inside your browser application, with independent concurrent SQL
sessions, OPFS persistence and static pgvector, pg_trgm and hstore. This is a preview wasm32 pthreads
package. Node.js and WASI execution are not supported.

```ts
import {Database} from '@postgamma/wasm';

const db = await Database.open({storage: {type: 'opfs', name: 'documents'}});
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

Serve the complete package asset directory over HTTPS or localhost. The page
must send `Cross-Origin-Opener-Policy: same-origin` and
`Cross-Origin-Embedder-Policy: require-corp`. Serve JavaScript with a JavaScript
MIME type and `.wasm` with `application/wasm`. Keep module Workers on the same
origin; retain all relative asset paths. A bundler must copy the complete package
directory rather than only importing its JS entry. Point `Database.open()` at
that copy with `assetBase: new URL('/postgamma/', location.href)` when needed.
The API and asset copy must come from the same package build.

For an unbundled page, map `@postgamma/wasm` to the deployed `src/index.js` using
an import map. All Worker, Wasm and initialization-resource paths then resolve
relative to that package. No compiler, Python, PostgreSQL installation or Arrow
runtime is required by the browser application.

Omit `storage` for an ephemeral database. OPFS requires Web Locks,
BroadcastChannel and synchronous file access handles; one origin/database name
has one live owner. Always close
the database to release its Workers and file handles. Clusters are target-specific;
do not mount a native PostgreSQL data directory. Synchronous COMMIT waits for the
browser storage barrier; hardware power-loss durability is not established.

Namespace checkpoints automatically reclaim metadata history. The initial
profile allows 16,384 live file/directory entries and bounds the two metadata
files to 16 MiB together. Existing preview databases upgrade when opened;
the migration needs room for one additional snapshot of at most 5 MiB.
Older builds cannot open the upgraded storage format.

`await db.dumpDataDir()` closes an idle database and returns a
`ReadableStream<Uint8Array>` containing its complete physical data directory.
Active requests and transactions reject the operation, leaving the database
usable. Consume the stream to EOF, cancel it, or call `db.close()` to release
the remaining storage ownership. Pipe it directly to a writable destination to
keep transfer memory bounded. Records contain at most 256 KiB of file data and
are protected by chained SHA-256 checksums and a required end record.

```ts
const archive = await db.dumpDataDir(); // closes the source
const restored = await Database.loadDataDir(archive, {
  storage: {type: 'opfs', name: 'documents-copy'},
});
// Alternatively: const restored = await db.clone({storage: ...});
```

Restore accepts a stream, for example `file.stream()` or a fetch response body.
Both operations accept an `AbortSignal`. The archive must come from the exact
same asset `buildId`, including extensions; it is not a cross-version upgrade
format. Restore requires a new OPFS name, or a new ephemeral module, and never
overwrites an existing database. Failed OPFS restores retain an incomplete
marker and cannot be opened; use a new name for a later attempt. `clone()` also
closes the source and streams into the new database. External tablespaces and
symbolic links are not supported by the archive format.

Four independent connections share the instance. A connection serializes
`query()` and `exec()` calls; different connections run concurrently. `exec()`
uses PostgreSQL script transaction semantics and accepts multiple statements;
`query()` accepts one statement with typed parameters. Use `execute()` and the
request's async iterator for bounded result batches, `cancel()` for cancellation,
`executeScript()` for streaming scripts, and the COPY stream for bulk data.
Values preserve exact int64/numeric strings,
NULL and binary `Uint8Array` data. Errors retain SQLSTATE and diagnostic fields.

If connection startup exceeds 90 seconds, the API discards the entire instance
and rejects callers after its cleanup attempt. Its existing connections become
unusable. Open a replacement instance before issuing further work; cleanup
failures remain visible in the error and diagnostics.

Values are bounded to 16 MiB, native materialized results to 24 MiB, and collected
results to 32 MiB including metadata. Set `chunkRows` to stream larger results.
Commands and each connection's query queue are bounded to 32 MiB of encoded
data; all Worker messages together are bounded to 64 MiB. Caller-retained
results are outside queue accounting.

Use `connection.onNotification(({channel, payload, pid}) => { ... }, onError)`
and SQL LISTEN/UNLISTEN for asynchronous notifications. The returned function
removes the callback. Idle listeners release their carriers, and delivery
follows transaction commit/rollback. The bounded native queue reports drops through
`NotificationOverflowError.dropped`; callback failures do not invalidate SQL.

Diagnostics sample on commands and lifecycle transitions by default.
For temporary profiling, pass `diagnostics: {sampleIntervalMs: 10}` to
`Database.open()`; `0` disables periodic samples. Sampling may miss transient
peaks. Idle command and result waits use notifications.

The default package includes pgvector 0.8.6, pg_trgm 1.6 and hstore 1.8. Run
`CREATE EXTENSION vector`, `CREATE EXTENSION pg_trgm` or `CREATE EXTENSION hstore`
once per database to enable each module. Trigram GUC settings are isolated by
logical connection. Arbitrary native extensions and multiple live
instances are outside the initial supported profile. Memory is bounded by the
included asset manifest (512 MiB shared linear memory); browser VM/RSS is separate.

The package version is derived from PostGamma's release version; for example,
`0.1.0a1` becomes npm `0.1.0-alpha.1`. `postgamma.assets.json` records the actual
target, build mode, source revisions, capabilities and asset hashes. Preserve
LICENSE, NOTICE and licenses/ when redistributing the package.
