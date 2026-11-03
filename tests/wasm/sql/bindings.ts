/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
// Independently implemented scenario adaptations; upstream engines are not test dependencies.
// PGlite: https://github.com/electric-sql/pglite/tree/ae182ff8bd5ba4acb887d6c925d607a1498aa0b5/packages/pglite/tests
//   basic.test.ts, array-types.test.ts, types.test.ts, message-context-leak.test.ts.
// DuckDB-Wasm: https://github.com/duckdb/duckdb-wasm/blob/def100b4be91a8ba27d441914e496231695ba0a8/packages/duckdb-wasm/test/batch_stream_async.test.ts
// SQLite: https://wasm-testing.sqlite.org/tester1.js
//   Reviewed SHA-256: 2d3aec037319395de4f4b37ea7bae3f050b55ab67a649d8dfe59be9838e5f814.
// chDB: https://github.com/chdb-io/chdb-core/blob/7d84d719da07184f6a49405a11b112f16925af72/packages/chdb-wasm/test/progress-cancel-fixture.html
import {DatabaseError} from '../src/api/database.js';
import type {Connection, Database, Result} from '../src/api/database.js';
import {assert, errorState, scalar} from './helpers.js';
import {collect} from './errors.js';

const rows = (results: Result[]) => results.flatMap(result => result.rows);
const equal = (actual: unknown, expected: unknown, message: string) =>
  assert(JSON.stringify(actual) === JSON.stringify(expected), message);

async function values(c: Connection, checks: string[]) {
  const elements = [null, 'NULL', '', 'comma,field', '{braces}', '"quoted"', '\\', '\u03c0-\ud83d\ude80'];
  const array = '{' + elements.map(value => value === null ? 'NULL' : '"' + value.replace(/["\\]/g, '\\$&') + '"').join(',') + '}';
  const output = await c.query('SELECT value FROM unnest($1::text[]) WITH ORDINALITY AS a(value, pos) ORDER BY pos', {
    parameters: [{typeOid: 1009, value: array}],
  });
  equal(rows(output).map(row => row[0]), elements, 'Array quoting and SQL NULL survive parameter/result transport');
  checks.push('pg-array-null-and-escapes');

  await c.query("CREATE TYPE binding_phase AS ENUM ('queued', 'applied')");
  const oid = Number(scalar(await c.query("SELECT typarray FROM pg_type WHERE typname = 'binding_phase'")));
  assert(Number.isSafeInteger(oid) && oid > 0, 'New enum array has a target-generated OID');
  equal(rows(await c.query('SELECT value::text FROM unnest($1::binding_phase[]) WITH ORDINALITY AS a(value, pos) ORDER BY pos', {
    parameters: [{typeOid: oid, value: '{queued,NULL,applied}'}],
  })), [['queued'], [null], ['applied']], 'A newly created array type uses its explicit OID');
  checks.push('pg-new-enum-array');

  const json = '{"exact":9007199254740993,"nested":[null,"NULL","\\u03c0"]}';
  equal(rows(await c.query("SELECT $1::jsonb->>'exact', $1::jsonb#>>'{nested,0}', $1::jsonb#>>'{nested,1}', $1::jsonb#>>'{nested,2}'", {
    parameters: [{typeOid: 3802, value: json}],
  })), [['9007199254740993', null, 'NULL', '\u03c0']], 'JSONB preserves exact numbers and null distinctions');
  checks.push('pg-jsonb-exact-and-null');

  const backing = new Uint8Array([91, 0, 17, 0, 128, 255, 92]);
  const binary = await c.query('SELECT $1::bytea, $2::bytea, $3::bytea', {
    binary: true, parameters: [{typeOid: 17, value: backing.subarray(1, 6)},
      {typeOid: 17, value: backing.subarray(3, 3)}, {typeOid: 17, value: null}],
  });
  backing.fill(33);
  const cells = rows(binary)[0];
  assert(cells[0] instanceof Uint8Array && cells[1] instanceof Uint8Array, 'Binary results retain their representation');
  equal([...cells[0]], [0, 17, 0, 128, 255], 'Only the input view is bound, including embedded NUL bytes');
  assert(cells[1].length === 0 && cells[2] === null, 'Empty subviews remain distinct from NULL');
  checks.push('sqlite-binary-subview');

  // PostgreSQL text cannot contain NUL. SQLite's accepted text case becomes a
  // rejection test here: silent shortening is never a valid representation.
  let rejected = false;
  try {await c.query('SELECT $1::text', {parameters: [{typeOid: 25, value: 'before\0after'}]});}
  catch (error) {rejected = error instanceof DatabaseError && error.status === 1;}
  assert(rejected, 'Embedded NUL in a text parameter is rejected instead of truncated');
  await errorState(() => c.query('SELECT $1::text', {
    parameters: [{typeOid: 25, value: new Uint8Array([97, 0, 98]), format: 1}],
  }), '22021');
  assert(scalar(await c.query('SELECT 29')) === '29', 'Text encoding failure leaves the connection usable');
  checks.push('sqlite-text-nul-rejected');

  equal(rows(await c.query("SELECT encode(convert_to('caf\u00e9\u00ff', 'LATIN1'), 'hex'), " +
    "convert_from(decode('636166e9ff', 'hex'), 'LATIN1')")), [['636166e9ff', 'caf\u00e9\u00ff']],
    'Statically bundled conversions preserve non-ASCII Latin-1 bytes in both directions');
  await errorState(() => c.query("SELECT convert_to('\u03c0', 'LATIN1')"), '22P05');
  await errorState(() => c.query("SELECT convert_from(decode('ff', 'hex'), 'UTF8')"), '22021');
  assert(scalar(await c.query("SELECT convert_from(convert_to('r\u00e9utilis\u00e9', 'LATIN1'), 'LATIN1')")) === 'r\u00e9utilis\u00e9',
    'Encoding errors preserve the connection and conversion cache');
  checks.push('latin1-conversion-roundtrip', 'encoding-error-reuse');

  const empty = await c.query('SELECT NULL::text AS repeated, 1::int8 AS repeated WHERE false');
  assert(rows(empty).length === 0, 'Empty results have no phantom row');
  const metadata = empty.find(result => result.columns.length > 0);
  assert(metadata, 'Empty results retain column metadata');
  equal(metadata.columns.map(column => [column.name, column.typeOid]), [['repeated', 25], ['repeated', 20]],
    'Duplicate aliases remain distinct ordinal columns on an empty result');
  checks.push('sqlite-empty-result-metadata');
}

async function streams(db: Database, c: Connection, checks: string[]) {
  const request = await c.execute(`SELECT g::int4 AS n, (9007199254740993::int8 + g) AS exact,
    (g::numeric / 1000)::numeric(12,3) AS decimal,
    CASE WHEN g % 5 = 0 THEN NULL ELSE g::text || '/\u03c0' END AS label
    FROM generate_series(1, 257) AS t(g) ORDER BY g`, {chunkRows: 17});
  let count = 0, dataBatches = 0;
  for await (const batch of request) {
    if (batch.rows.length) dataBatches++;
    assert(batch.rows.length <= 17, 'Mixed rows obey the requested batch limit');
    equal(batch.columns.map(column => column.typeOid), [23, 20, 1700, 25], 'Every batch retains ordered column types');
    for (const row of batch.rows) {
      const n = ++count;
      equal(row, [String(n), String(9007199254740993n + BigInt(n)), (n / 1000).toFixed(3), n % 5 ? `${n}/\u03c0` : null],
        'Mixed-type rows retain column alignment and order across batches');
    }
  }
  assert(count === 257 && dataBatches === 16, 'All mixed rows arrive exactly once');
  checks.push('duckdb-mixed-batch-columns');

  const sql = "SELECT g, repeat('r', 256) FROM generate_series(1, 100000) AS t(g)";
  for await (const batch of await c.execute(sql, {chunkRows: 8})) {
    assert(batch.rows.length === 8, 'Iterator produces a first batch');
    break;
  }
  assert((await db.telemetry()).requests === 0n && scalar(await c.query('SELECT 31')) === '31',
    'Breaking the iterator retires its native request before connection reuse');
  checks.push('duckdb-iterator-break-cleanup');
  const sentinel = new Error('consumer stopped processing');
  let caught: unknown;
  try {for await (const _batch of await c.execute(sql, {chunkRows: 8})) throw sentinel;}
  catch (error) {caught = error;}
  assert(caught === sentinel && (await db.telemetry()).requests === 0n,
    'Consumer failure releases its request and preserves the original exception');
  assert(scalar(await c.query('SELECT 37')) === '37', 'Connection is reusable after consumer failure');
  checks.push('duckdb-consumer-error-cleanup');
}

async function copy(c: Connection, checks: string[]) {
  await c.query('CREATE TABLE binding_csv(id int PRIMARY KEY, value text)');
  const input = new TextEncoder().encode('41,"line\nbreak"\r\n42,"comma,quote"""\r\n43,\r\n44,""\r\n45,"pi-\u03c0-\ud83d\ude80"\r\n');
  const request = await c.execute('COPY binding_csv FROM STDIN WITH (FORMAT csv)');
  const stream = (await request.next())?.copy;
  assert(stream?.direction === 'in', 'CSV input starts a COPY stream');
  // One-byte views split every multibyte character and quote/CRLF pair.
  for (let at = 0; at < input.length; at++) await stream.write(input.subarray(at, at + 1));
  await stream.finish(); await stream.close();
  assert((await collect(request))[0].command === 'COPY 5', 'Split CSV input has one successful terminal result');
  equal(rows(await c.query('SELECT value FROM binding_csv ORDER BY id')),
    [['line\nbreak'], ['comma,quote"'], [null], [''], ['pi-\u03c0-\ud83d\ude80']], 'CSV byte boundaries preserve quotes, newlines, NULL and Unicode');
  const out = await c.execute('COPY (SELECT * FROM binding_csv ORDER BY id) TO STDOUT WITH (FORMAT csv)');
  const reader = (await out.next())?.copy;
  assert(reader?.direction === 'out', 'CSV output starts a COPY stream');
  const bytes: number[] = [];
  for (;;) {const part = await reader.read(1); if (!part) break; assert(part.length === 1, 'COPY honors one-byte reads'); bytes.push(part[0]);}
  await reader.close();
  assert((await collect(out))[0].command === 'COPY 5', 'CSV output terminates after all bytes');
  equal(new TextDecoder('utf-8', {fatal: true}).decode(new Uint8Array(bytes)),
    '41,"line\nbreak"\n42,"comma,quote"""\n43,\n44,""\n45,pi-\u03c0-\ud83d\ude80\n', 'COPY output roundtrips through single-byte reads');
  checks.push('pglite-csv-byte-boundaries');
}

async function computation(db: Database, c: Connection, checks: string[]) {
  const observer = await db.connect('binding-observer');
  try {
    await c.query("SET statement_timeout = '5s'");
    const pid = (await c.status()).pid;
    const sql = 'SELECT sum(g::numeric * g::numeric) FROM generate_series(1, 1000000000) AS t(g)';
    const request = await c.execute(sql);
    const outcome = collect(request).then(value => value, error => error);
    let active = false;
    for (let attempt = 0; attempt < 100; attempt++) {
      active = scalar(await observer.query("SELECT count(*) FROM pg_stat_activity WHERE pid = $1::int AND state = 'active' AND query = $2::text", {
        parameters: [{typeOid: 23, value: String(pid)}, {typeOid: 25, value: sql}],
      })) === '1';
      if (active) break;
      await new Promise(resolve => setTimeout(resolve, 5));
    }
    assert(active, 'The canceled request is actively executing a CPU-bound query');
    const started = performance.now();
    await request.cancel();
    const result = await outcome;
    assert(result instanceof DatabaseError && result.sqlstate === '57014' && result.message.includes('user request'),
      'Active computation stops because of explicit cancellation');
    await c.query('SET statement_timeout = 0');
    assert(scalar(await c.query('SELECT 43')) === '43', 'A canceled computation leaves the next request intact');
    (window as unknown as {probeEvents: unknown[]}).probeEvents.push({kind: 'compute-cancel', elapsedMs: performance.now() - started, pid});
    checks.push('chdb-active-compute-cancel');
  } finally {await observer.close();}
}

async function messageMemory(db: Database, c: Connection, checks: string[]) {
  const payload = 'p'.repeat(64 * 1024);
  const sql = "SELECT octet_length('" + payload + "')";
  const sample = async () => {
    const value = rows(await c.query("SELECT count(*), coalesce(sum(used_bytes), 0) FROM pg_backend_memory_contexts WHERE name = 'MessageContext'"))[0];
    assert(Number(value[0]) > 0, 'MessageContext is observable in the current backend');
    return Number(value[1]);
  };
  for (let i = 0; i < 4; i++) await c.query(sql);
  const observed = [await sample()];
  for (let round = 0; round < 3; round++) {
    for (let i = 0; i < 16; i++) assert(scalar(await c.query(sql)) === String(payload.length), 'Repeated large query completes');
    observed.push(await sample());
  }
  // Each window sends 1 MiB; tolerate two message payloads after warmup, not
  // growth proportional to the number of queries. This measures PG contexts,
  // since an allocator may retain Wasm pages after freeing their contents.
  assert(observed.every(bytes => bytes <= observed[0] + 2 * payload.length), 'MessageContext is reset between large queries');
  assert((await db.telemetry()).requests === 0n, 'Repeated queries leave no live requests');
  (window as unknown as {probeEvents: unknown[]}).probeEvents.push({kind: 'message-context-reset', usedBytes: observed,
    queries: 52, payloadBytes: payload.length});
  checks.push('pglite-message-context-reset');
}

export async function bindings(db: Database, checks: string[]) {
  const c = await db.connect('binding-regressions');
  try {
    await values(c, checks);
    await streams(db, c, checks);
    await copy(c, checks);
    await computation(db, c, checks);
    await messageMemory(db, c, checks);
  } finally {await c.close();}
}
