/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
import {Database, DatabaseError, ClosedError} from '../src/api/database.js';
import type {Connection, LogicalOptions} from '../src/api/database.js';
import {assert, scalar} from './helpers.js';
const outcome = (work: Promise<unknown>) => work.then(() => undefined, error => error);
const phase = (name: string) => (window as unknown as {probeEvents: unknown[]}).probeEvents.push({kind: 'logical-phase', phase: name});
async function dump(db: Database, options: LogicalOptions = {}) {
  const chunks: Uint8Array[] = [];
  await db.dumpLogical(new WritableStream({write(chunk) {chunks.push(chunk);}}), options);
  return new Blob(chunks);
}
async function settledAllocations(db: Database) {
  // Detached pthread retirement and backend cleanup follow the operation's
  // completion message. Compare quiescent allocations, retaining a bounded
  // wait instead of mixing in-flight teardown with idle measurements.
  const deadline = performance.now() + 2000;
  let previous = -1, stable = 0;
  while (performance.now() < deadline) {
    const value = await db.telemetry();
    if (value.running === 0n && value.requests === 0n && value.queued === 0n && value.runnable === 0n &&
        value.allocatorLiveBytes === previous) stable++;
    else stable = 0;
    previous = value.allocatorLiveBytes;
    if (stable >= 4) return previous;
    await new Promise(resolve => setTimeout(resolve, 10));
  }
  throw new Error('Logical operation allocation sampling did not quiesce');
}
const fingerprint = async (c: Connection) => scalar(await c.query("SELECT md5(string_agg(row_to_json(i)::text, E'\\n' ORDER BY id)) FROM logical.items i"));
async function verify(c: Connection, expected: unknown) {
  assert(scalar(await c.query('SELECT count(*) FROM logical.items')) === '5000', 'Restored row count');
  assert(await fingerprint(c) === expected, 'Every restored row and column matches the snapshot');
  const row = (await c.query(`SELECT id::text, exact::text, payload::text, encode(bytes,'hex'), missing,
    labels::text, state::text FROM logical.items WHERE id=1`))[0].rows[0];
  assert(JSON.stringify(row) === JSON.stringify(['1', '12345678901234567890.123456789', '雪 🐘', '00017f80ff', null,
    '[0:2]={雪,NULL,""}', 'ready']), 'Logical exact values, bytea, arrays, enum and NULL');
  assert(scalar(await c.query('SELECT logical.plus_one(8)')) === '9', 'Function definition restored');
  assert(scalar(await c.query('SELECT count(*) FROM logical.visible')) === '5000', 'View definition restored');
  assert(scalar(await c.query("SELECT last_value FROM logical.sequence")) === '71', 'Sequence state restored');
  assert(scalar(await c.query("SELECT data->'unicode' FROM logical.extension_values")) === '雪', 'Extension type data restored');
  assert(scalar(await c.query("SELECT obj_description('logical.items'::regclass)")) === '逻辑备份', 'Comment restored');
  assert(scalar(await c.query("SELECT count(*) FROM pg_indexes WHERE schemaname='logical' AND indexname='items_payload'")) === '1', 'Secondary index restored');
  assert(scalar(await c.query("SELECT encode(lo_get(45001),'hex')")) === '0001027f80ff', 'Large object content restored');
}
export async function logical(checks: string[], metrics: unknown[]) {
  const db = await Database.open(), c = await db.connect();
  const native = await fetch('logical-native.dump'); assert(native.ok, 'Pinned native pg_dump archive is available');
  const nativeArchive = await native.blob();
  await db.restoreLogical(nativeArchive);
  const imported = (await c.query("SELECT id::text, exact::text, encode(bytes,'hex'), value, missing, phase::text FROM interop.view"))[0].rows[0];
  assert(JSON.stringify(imported) === JSON.stringify(['9223372036854775807', '12345678901234567890.123456789', '007f80ff', '雪 🐘', null, 'ready']),
    'Native pg_dump restores exact values in Wasm');
  const nativeExport = new Uint8Array(await (await dump(db)).arrayBuffer());
  (window as unknown as {probeEvents: unknown[]}).probeEvents.push({kind: 'logical-native-interop',
    archive: btoa(Array.from(nativeExport, byte => String.fromCharCode(byte)).join(''))});
  checks.push('logical-native-pg-dump-import', 'logical-native-pg-restore-export');
  phase('interop');
  await c.exec(`CREATE EXTENSION hstore; CREATE EXTENSION pg_trgm;
    CREATE SCHEMA logical; CREATE TYPE logical.status AS ENUM ('ready','done');
    CREATE SEQUENCE logical.sequence START 71;
    CREATE TABLE logical.items(id bigint PRIMARY KEY, exact numeric, payload text, bytes bytea, missing text,
      labels text[], state logical.status);
    INSERT INTO logical.items SELECT i, 12345678901234567890.123456789,
      CASE WHEN i=1 THEN '雪 🐘' ELSE repeat(md5(i::text),8) END, decode('00017f80ff','hex'), NULL,
      '[0:2]={雪,NULL,""}'::text[], 'ready' FROM generate_series(1,5000) i;
    CREATE INDEX items_payload ON logical.items USING gin(payload gin_trgm_ops);
    COMMENT ON TABLE logical.items IS '逻辑备份';
    CREATE VIEW logical.visible AS SELECT * FROM logical.items;
    CREATE FUNCTION logical.plus_one(integer) RETURNS integer LANGUAGE sql IMMUTABLE AS 'SELECT $1+1';
    CREATE TABLE logical.extension_values(data hstore);
    INSERT INTO logical.extension_values VALUES ('unicode=>雪,missing=>NULL');
    CREATE TABLE public.zz_conflict(id int); INSERT INTO public.zz_conflict VALUES (77);
    CREATE TEMP TABLE omitted_temp(id int);
    SELECT lo_from_bytea(45001,decode('0001027f80ff','hex'))`);
  const expected = await fingerprint(c);
  const parts: Uint8Array[] = []; let peer = false;
  await db.dumpLogical(new WritableStream({async write(chunk) {
    if (!peer) {
      peer = true;
      assert(scalar(await c.query('SELECT 8')) === '8', 'SQL continues while backup sink applies backpressure');
      await c.query('INSERT INTO logical.items(id) VALUES (5001)');
    }
    parts.push(chunk);
  }}));
  const full = new Blob(parts);
  assert(full.size > 1024 * 1024 && await full.slice(0, 5).text() === 'PGDMP', 'Standard custom archive, streamed over bounded chunks');
  assert(parts.every(part => part.length <= 16384), 'Archive bridge chunk bound');
  assert(scalar(await c.query('SELECT count(*) FROM logical.items')) === '5001', 'Source remains open and writable');
  await c.query('DELETE FROM logical.items WHERE id=5001');
  checks.push('logical-custom-format', 'logical-bounded-chunks', 'logical-online-peer-progress');
  phase('snapshot');

  const extra = await Promise.all([db.connect(), db.connect(), db.connect()]);
  assert(await outcome(dump(db)) instanceof DatabaseError, 'Four live sessions reject logical admission');
  await Promise.all(extra.map(connection => connection.close()));
  const blocked = new AbortController(); let entered!: () => void;
  const sinkEntered = new Promise<void>(resolve => {entered = resolve;});
  const canceled = outcome(db.dumpLogical(new WritableStream({write() {entered(); return new Promise(() => {});}}), {signal: blocked.signal}));
  await Promise.race([sinkEntered, canceled.then(error => {throw error ?? new Error('Backup ended before the sink was reached');})]);
  assert(await outcome(dump(db)) instanceof Error, 'Concurrent logical tools rejected');
  const reason = new Error('cancel logical sink'); blocked.abort(reason);
  assert(await canceled === reason, 'Cancellation keeps caller reason and drains tool');
  await c.exec('BEGIN; LOCK logical.items IN ACCESS EXCLUSIVE MODE');
  assert((await outcome(dump(db, {timeoutMs: 750})))?.name === 'TimeoutError', 'Cancel backup blocked on a database lock');
  await c.query('ROLLBACK');
  const sinkError = new Error('logical destination failed');
  assert(await outcome(db.dumpLogical(new WritableStream({write() {throw sinkError;}}))) === sinkError, 'Destination error retires the tool');
  const schema = await dump(db, {schemaOnly: true}), data = await dump(db, {dataOnly: true});
  assert(await outcome(dump(db, {database: 'missing_database'})) instanceof DatabaseError, 'Missing database releases startup and archive ownership');
  assert(schema.size < full.size && data.size > 1024 * 1024, 'Repeated invocations reset pg_dump options and state');
  assert((await db.telemetry()).connections === 1n, 'Logical sessions retire before completion');
  phase('cancellation');
  const allocations: number[] = [];
  for (let round = 0; round < 24; round++) {
    await db.dumpLogical(new WritableStream({write() {}}), {schemaOnly: round % 2 === 0});
    assert(await outcome(db.dumpLogical(new WritableStream({write() {throw sinkError;}}))) === sinkError,
      'Every repeated sink failure retires the tool');
    allocations.push(await settledAllocations(db));
  }
  (window as unknown as {probeEvents: unknown[]}).probeEvents.push({kind: 'logical-memory', allocations});
  assert(Math.max(...allocations.slice(8)) - Math.min(...allocations.slice(8)) < 8 * 1024,
    'Repeated successful and failed dumps release invocation allocations after warmup');
  phase('repetition');
  checks.push('logical-session-admission', 'logical-single-tool-admission', 'logical-cancel-sink', 'logical-lock-timeout',
    'logical-sink-error', 'logical-invocation-state', 'logical-missing-database', 'logical-allocation-release', 'logical-source-session-release');
  await c.close(); metrics.push(await db.close());

  const storage = {type: 'opfs' as const, name: 'logical-' + crypto.randomUUID()};
  const target = await Database.open({storage}), t = await target.connect();
  // Single transaction must undo schema creation and COPY data on late failure.
  await t.exec('CREATE TABLE public.zz_conflict(sentinel text); INSERT INTO public.zz_conflict VALUES (\'keep\')');
  const conflict = await outcome(target.restoreLogical(full));
  assert(conflict instanceof DatabaseError && conflict.message.length > 0, 'Restore surfaces pg_restore diagnostics');
  assert(scalar(await t.query("SELECT count(*) FROM pg_namespace WHERE nspname='logical'")) === '0' &&
    scalar(await t.query('SELECT sentinel FROM public.zz_conflict')) === 'keep', 'Failed restore rolls back DDL and preserves target');
  await t.query('DROP TABLE public.zz_conflict');
  assert(await outcome(target.restoreLogical(full.slice(0, full.size - 1024))) instanceof DatabaseError, 'Truncated data fails restore');
  assert(scalar(await t.query("SELECT count(*) FROM pg_namespace WHERE nspname='logical'")) === '0', 'Truncated restore rolls back');
  assert(await outcome(target.restoreLogical(new Blob(['bad archive']))) instanceof DatabaseError, 'Invalid header rejected');
  checks.push('logical-restore-conflict-rollback', 'logical-truncation-rollback', 'logical-invalid-header');

  const sourceError = new Error('logical source failed'); let sent = false;
  const first = new Uint8Array(await full.slice(0, 32768).arrayBuffer());
  assert(await outcome(target.restoreLogical(new ReadableStream<Uint8Array>({pull(controller) {
    if (!sent) {sent = true; controller.enqueue(first);} else controller.error(sourceError);
  }}))) === sourceError, 'Restore preserves source error');
  assert((await outcome(target.restoreLogical(new ReadableStream<Uint8Array>({
    start(controller) {controller.enqueue(first);}, pull() {return new Promise(() => {});},
  }), {timeoutMs: 750})))?.name === 'TimeoutError', 'Deadline cancels a stalled restore source');
  assert(scalar(await t.query("SELECT count(*) FROM pg_namespace WHERE nspname='logical'")) === '0', 'Source error and cancellation roll back');
  checks.push('logical-source-error', 'logical-source-timeout', 'logical-cancel-rollback');
  phase('restore-errors');

  await target.restoreLogical(schema);
  assert(scalar(await t.query('SELECT count(*) FROM logical.items')) === '0', 'Schema-only restore');
  await target.restoreLogical(data); await verify(t, expected);
  await t.query('DELETE FROM logical.items WHERE id=2');
  await target.restoreLogical(full, {clean: true}); await verify(t, expected);
  assert(scalar(await t.query("SELECT count(*) FROM pg_class WHERE relname='omitted_temp'")) === '0', 'Temp objects excluded');
  assert((await target.telemetry()).connections === 1n, 'Restore releases internal connection');
  checks.push('logical-schema-only', 'logical-data-only', 'logical-clean-restore', 'logical-consistent-snapshot',
    'logical-exact-values', 'logical-all-row-values', 'logical-large-objects', 'logical-schema-objects', 'logical-extension-data', 'logical-temp-excluded', 'logical-target-session-release');
  phase('restore');
  const restoreAllocations: number[] = [];
  for (let round = 0; round < 16; round++) {
    await target.restoreLogical(nativeArchive, {clean: true});
    if (round % 3 === 0) {
      assert(await outcome(target.restoreLogical(nativeArchive.slice(0, nativeArchive.size - 16), {clean: true})) instanceof DatabaseError,
        'Repeated truncated restore fails and rolls back');
    }
    restoreAllocations.push(await settledAllocations(target));
  }
  (window as unknown as {probeEvents: unknown[]}).probeEvents.push({kind: 'logical-restore-memory', allocations: restoreAllocations});
  assert(Math.max(...restoreAllocations.slice(6)) - Math.min(...restoreAllocations.slice(6)) < 256 * 1024,
    'Repeated successful and failed restores release invocation allocations after warmup');
  assert(scalar(await t.query('SELECT id::text FROM interop.view')) === '9223372036854775807', 'Repeated restore keeps exact data');
  checks.push('logical-restore-allocation-release');
  phase('restore-repetition');
  await t.close(); metrics.push(await target.close());
  const reopened = await Database.open({storage, create: false}), r = await reopened.connect();
  await verify(r, expected); await r.close();
  let began!: () => void; const begin = new Promise<void>(resolve => {began = resolve;});
  const duringClose = outcome(reopened.dumpLogical(new WritableStream({write() {began(); return new Promise(() => {});}})));
  await Promise.race([begin, duringClose.then(error => {throw error ?? new Error('Backup ended before close was exercised');})]);
  metrics.push(await reopened.close());
  assert(await duringClose instanceof ClosedError, 'Database.close cancels and joins logical cleanup');
  checks.push('logical-opfs-reopen', 'logical-concurrent-close', 'logical-resource-release');
  phase('closed');
}
