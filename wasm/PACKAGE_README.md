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

Run `npx --no-install postgamma-assets public/postgamma` after installing the
package. The helper verifies asset hashes before copying the complete runtime,
including Workers, initialization data and license notices. Repeated builds
update files owned by the helper and preserve unrelated files; modified asset
files cause an error. Complete the copy before serving the destination.

For Vite, call the helper from your configuration:

```js
import {copyAssets, isolationHeaders} from '@postgamma/wasm/tooling';
await copyAssets('public/postgamma');
export default {
  server: {headers: isolationHeaders},
  preview: {headers: isolationHeaders},
  optimizeDeps: {exclude: ['@postgamma/wasm']},
  build: {target: 'es2022'},
};
```

The repository's `examples/wasm/bundlers` contains a working page and Vite and
webpack configuration examples. Copy `app.js` and `index.html` with the chosen
configuration into an application containing the installed package and bundler.
Run `vite`, `vite build`, `vite preview`, or `webpack serve` / `webpack`.
For webpack, also install webpack-cli and webpack-dev-server. Production hosting
must send the same isolation headers and serve the generated asset directory;
development-server configuration does not configure your production host.

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

For logical backups, use `dumpLogical(destination, options)` and
`restoreLogical(source, options)`. The destination is a
`WritableStream<Uint8Array>`; the source is a `Blob`, `File`, or
`ReadableStream<Uint8Array>`. Export writes an uncompressed PostgreSQL custom
archive (`PGDMP`) using the bundled `pg_dump`. Restore uses `pg_restore` and
always runs in a single transaction, rolling back schema and table changes on
archive or SQL errors. PostgreSQL sequence changes to preexisting sequences
are not transactional; use a fresh target for recovery that must leave no
partial sequence changes on failure.

```ts
await db.dumpLogical(await fileHandle.createWritable(), {timeoutMs: 60_000});
const target = await Database.open({storage: {type: 'opfs', name: 'imported'}});
await target.restoreLogical(await fileHandle.getFile(), {timeoutMs: 60_000});
```

Logical export keeps the database open and takes a PostgreSQL consistent
snapshot. The transfer observes stream backpressure and accepts `signal` and
`timeoutMs`; `db.close()` cancels active logical work and waits for cleanup.
One logical operation is admitted at a time and consumes one of the four SQL
sessions. Close an idle connection first if all four slots are occupied.
Options include `schemaOnly`, `dataOnly`, `noOwner`, `noPrivileges`, and
`database`/`user` (defaults: `postgres`/`postgamma`). Restore additionally accepts
`clean: true` to replace archived objects already present in the target.
The default target should be empty; incompatible existing objects cause failure.

Logical archives do not require the same Wasm asset `buildId`. PostgreSQL
version and extension compatibility still apply, and extension binaries and
referenced roles must exist on the destination. This backs up one database's
schema and data, including sequences, functions and installed extension data;
it does not dump cluster-wide roles, tablespaces, or temporary objects.
Only custom-format archives with no compression are supported in this build.
Use a separately retained checksum if archive integrity verification is required.
`db.telemetry().allocatorLiveBytes` reports live allocations across the module;
the Wasm memory size also includes already-freed space kept for reuse.

Four independent connections share the instance. A connection serializes
`query()` and `exec()` calls; different connections run concurrently. `exec()`
uses PostgreSQL script transaction semantics and accepts multiple statements;
`query()` accepts one statement with typed parameters. Use `execute()` and the
request's async iterator for bounded result batches, `cancel()` for cancellation,
`executeScript()` for streaming scripts, and the COPY stream for bulk data.
Values preserve exact int64/numeric strings,
NULL and binary `Uint8Array` data. Errors retain SQLSTATE and diagnostic fields.

`Codecs` optionally decodes those raw values and builds typed parameters:

```ts
import {Codecs} from '@postgamma/wasm';
const codecs = new Codecs();
const raw = await connection.query('SELECT $1::int8', {
  parameters: [codecs.parameter(20, 9007199254740993n)],
});
const decoded = raw.map(result => codecs.decode(result)); // bigint value
```

Booleans, int2/int4/OID, floats and bytea become boolean, number or Uint8Array;
int8 becomes bigint. Numeric and temporal values remain exact PostgreSQL text.
JSON also remains text by default; `new Codecs({json: 'javascript'})` opts into
JSON.parse and its number precision limits. `parameter()` serializes JSON values
with JSON.stringify; use a raw `Parameter` to submit already serialized JSON.
Date parameters are accepted only for timestamptz and use UTC ISO text.

Text arrays decode to `{values, lowerBounds, dimensions}`, retaining SQL NULL,
quoted strings and multidimensional bounds. `parameter()` accepts ordinary
rectangular arrays or that object. Common binary scalar types are decoded;
unsupported binary representations, including arrays, remain Uint8Array.
Unknown text types remain strings. Each registry can register custom codecs by
PostgreSQL type OID with `register(oid, {decode, encode})`; registrations are local
to that object. Raw query results and other registries remain unchanged.

Use a named prepared statement to reuse parsing and planning:

```ts
const statement = await connection.prepare('SELECT $1::int8 AS id, $2::text AS label');
try {
  console.log(statement.parameterTypes); // [20, 25], inferred by PostgreSQL
  const result = await statement.query({parameters: [
    {typeOid: 20, value: '9007199254740993'}, {typeOid: 25, value: 'example'},
  ]});
} finally {await statement.close();}
```

`prepare(sql, {parameterTypes: [...]})` optionally supplies type OIDs; zero asks
PostgreSQL to infer that parameter. Up to 64 parameters are supported.
`statement.columns` describes the result without executing it. `query()` collects
bounded results and `execute({chunkRows: ...})` returns the ordinary request
iterator, including binary results and cancellation. Prepare, query and script
calls share connection ordering. Parse/Describe yields the driver while waiting
for PostgreSQL; other sessions stay responsive. Closing a connection also closes
its statements. A statement cannot be reused after close starts.

SQL, scripts and prepared statements accept `{signal, timeoutMs}`. A deadline
includes time spent waiting in the connection queue; `0` rejects immediately.
Cancellation removes a pending command without running it. An active command is
canceled and its native request is retired before collected calls reject with
the signal's reason, or a `TimeoutError` for a deadline. Streaming requests also
close automatically on abort; await `request.close()` before reusing the
connection directly. Cancellation does not undo an already committed operation.

```ts
await connection.transaction(async tx => {
  await tx.query('INSERT INTO accounts(id, balance) VALUES (1, 100)');
  await tx.query('UPDATE accounts SET balance = balance - 10 WHERE id = 1');
}, {isolation: 'serializable', timeoutMs: 5000});
```

The callback owns the connection through COMMIT or ROLLBACK. Use `tx.query()` or
`tx.exec()` inside it; ordinary connection queries wait until it finishes and
direct `execute()` calls reject with `ConnectionBusyError`. The callback's return
value is delivered after COMMIT. Exceptions, cancellation and failed transaction
state trigger ROLLBACK before rejection. SAVEPOINT recovery is supported; do not
issue BEGIN, COMMIT or ROLLBACK for the outer transaction inside the callback.
The callback handle expires on completion. Cancellation cannot stop arbitrary
application JavaScript, but it retires that handle and rolls back the database
work. `readOnly` and the three PostgreSQL isolation levels are supported.

`copyFrom(sql, source, options)` streams a File, Blob or ReadableStream<Uint8Array>
into `COPY ... FROM STDIN`. `copyTo(sql, destination, options)` streams
`COPY ... TO STDOUT` into a WritableStream<Uint8Array>, including a file writable
handle. They honor backpressure and share connection ordering, AbortSignal and
deadline behavior. Each returns PostgreSQL's terminal COPY result after cleanup;
copyTo also closes the destination on success and aborts it on failure.

```ts
await connection.copyFrom('COPY measurements FROM STDIN (FORMAT csv, HEADER)', file);
const response = await fetch('/data/measurements.csv');
if (!response.ok || !response.body) throw new Error('Download failed');
await connection.copyFrom('COPY measurements FROM STDIN (FORMAT csv, HEADER)', response.body);
await connection.copyTo('COPY measurements TO STDOUT (FORMAT csv, HEADER)', writable);
```

These helpers use PostgreSQL's COPY formats and do not infer a schema or parse
Parquet. Use a normal fetch AbortSignal for the network request itself. Source
and sink exceptions retire the database request before rejection. A deadline
also interrupts a stalled application stream without waiting forever for its
custom cancel/abort callback. Application-owned buffers remain outside database
queue accounting.

Parallel queries are opt-in on each connection:

```ts
await connection.exec('SET max_parallel_workers = 2; SET max_parallel_workers_per_gather = 2');
```

The supported budget is two parallel workers shared by the four sessions, in
addition to query leaders. Keep `parallel_leader_participation = on` (the
PostgreSQL default). The optimizer selects suitable parallel plans; when both
workers are occupied, queries fall back to serial execution. Acceptance requires
`Workers Planned: 2` and `Workers Launched: 2` in `EXPLAIN ANALYZE`.
New connections start with parallel workers disabled. The worker budget is
also used by parallel index maintenance; larger worker counts are unvalidated.

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
