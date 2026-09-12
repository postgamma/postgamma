/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
import {Database, ClosedError, DatabaseError} from '../src/api/database.js';
import type {Writer} from '../src/protocol/wire.js';
import {assert, scalar} from './helpers.js';
import {waitLock} from './concurrency.js';
export async function lifecycle(db: Database, checks: string[]) {
  const a = await db.connect('close-holder'), b = await db.connect('close-waiter'), c = await db.connect('close-observer');
  await a.query('CREATE TABLE close_test(id int PRIMARY KEY)'); await a.query('INSERT INTO close_test VALUES (1)');
  await a.query('BEGIN'); await a.query('SELECT * FROM close_test FOR UPDATE');
  let settlements = 0;
  const blocked = b.query('SELECT * FROM close_test FOR UPDATE').then(
    value => {settlements++; return value;}, error => {settlements++; return error;});
  await waitLock(c, (await b.status()).pid);
  await Promise.all([b.close(), b.close()]);
  const closed = await blocked;
  assert(closed instanceof ClosedError || closed instanceof DatabaseError && closed.sqlstate === '57014', 'Blocked query is terminated by connection close');
  assert(settlements === 1 && (await a.status()).transaction === 2 && scalar(await c.query('SELECT 1')) === '1', 'Connection close leaves A/C unaffected');
  await a.query('ROLLBACK');
  const rows = await a.execute("SELECT g, repeat('q', 1024) FROM generate_series(1, 100000) g", {chunkRows: 32});
  await rows.next();
  const copy = await c.execute("COPY (SELECT g, repeat('q', 4096) FROM generate_series(1, 100000) g) TO STDOUT");
  const stream = (await copy.next())!.copy!; await stream.read();
  await new Promise(resolve => setTimeout(resolve, 100));
  const firstClose = db.close(), repeatedClose = db.close();
  assert(firstClose === repeatedClose, 'Instance close shares one completion');
  const metrics = await firstClose;
  assert(settlements === 1, 'No duplicate query settlement');
  let rejected = 0;
  for (const operation of [() => db.connect(), () => a.query('SELECT 1'), () => rows.next(), () => stream.read()]) {
    try {await operation();} catch {rejected++;}
  }
  assert(rejected === 4, 'Closed descendants reject new operations');
  checks.push('close-blocked-query', 'idempotent-close', 'close-full-result-and-copy', 'closed-descendants');
  const fresh = await Database.open(), connection = await fresh.connect();
  assert(scalar(await connection.query('SELECT 7')) === '7', 'New module starts after full close');
  // Close while a query is being submitted, before its request handle arrives.
  const connecting = fresh.connect('close-during-connect');
  const starting = connection.execute('SELECT pg_sleep(2)').then(async request => {
    try {await request.next();} finally {await request.close();}
  }).then(() => 'done', error => error);
  await fresh.close(); await starting;
  const late = await connecting;
  let lateRejected = false; try {await late.query('SELECT 1');} catch {lateRejected = true;}
  assert(lateRejected, 'Connection created during close is also retired');
  (window as unknown as {probeEvents: unknown[]}).probeEvents.push({kind: 'lifecycle', metrics,
    recreated: fresh.diagnostics(), settlements});
  checks.push('close-during-submission', 'recreate-after-close');
}
export async function fatal(db: Database, checks: string[], fault: number) {
  const a = await db.connect('fatal-a'), b = await db.connect('fatal-b');
  const waiters = [a.query('SELECT pg_sleep(20)'), b.query('SELECT pg_sleep(20)'),
    a.query('SELECT 1'), b.query('SELECT 2')];
  const counts = waiters.map(() => 0);
  const settled = waiters.map((work, index) => work.then(() => {counts[index]++; return 'success';}, error => {counts[index]++; return error;}));
  await db.telemetry();
  // Private test opcode is compiled only for the browser check action. It
  // traps/aborts inside the real Wasm host pthread, not a fabricated JS error.
  const transport = (db as unknown as {transport: {call(op: number, handle: undefined, write: (w: Writer) => void): Promise<unknown>}}).transport;
  const injected = transport.call(65535, undefined, w => w.u32(fault)).then(() => 'success', error => error);
  const outcomes = await Promise.all([...settled, injected]);
  assert(outcomes.every(value => value instanceof Error) && counts.every(count => count === 1), 'Fatal error settles every in-flight and queued Promise once');
  const metrics = db.diagnostics() as {terminationRequested: boolean; interruptedWorkers: number; runningWorkers: number};
  assert(metrics.terminationRequested && metrics.interruptedWorkers > 0 && metrics.runningWorkers === 0, 'Fatal path requests termination of all owned SDK workers');
  let rejected = false; try {await a.query('SELECT 3');} catch {rejected = true;}
  assert(rejected, 'Failed instance rejects subsequent requests');
  const fresh = await Database.open(), connection = await fresh.connect();
  assert(scalar(await connection.query('SELECT 8')) === '8', 'Fresh module runs after a fatal instance');
  await fresh.close();
  (window as unknown as {probeEvents: unknown[]}).probeEvents.push({kind: 'fatal', fault, metrics,
    recreated: fresh.diagnostics(), settlements: counts});
  checks.push(fault === 1 ? 'real-wasm-trap' : 'real-wasm-abort', 'fatal-settles-all', 'fatal-instance-rejected', 'recreate-after-fatal');
}
