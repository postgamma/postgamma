/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
import type {Database, Connection} from '../src/api/database.js';
import {DatabaseError} from '../src/api/database.js';
import {assert, errorState, scalar} from './helpers.js';
export async function waitLock(observer: Connection, pid: number) {
  const deadline = performance.now() + 5000;
  while (performance.now() < deadline) {
    const value = scalar(await observer.query(`SELECT EXISTS (SELECT FROM pg_stat_activity a
      WHERE pid = ${pid} AND wait_event_type = 'Lock' AND EXISTS
      (SELECT FROM pg_locks l WHERE l.pid = a.pid AND NOT granted))::text`));
    if (value === 'true') {
      (window as unknown as {probeEvents: unknown[]}).probeEvents.push({kind: 'kernel-lock-wait', pid,
        waitEventType: 'Lock', ungrantedLock: true});
      return;
    }
    await new Promise(resolve => setTimeout(resolve, 5));
  }
  throw new Error(`Backend ${pid} did not enter a real PostgreSQL lock wait`);
}
export async function concurrency(db: Database, checks: string[]) {
  const a = await db.connect('wasm-a'), b = await db.connect('wasm-b'), c = await db.connect('wasm-c');
  await a.query('CREATE TABLE session_test(id int PRIMARY KEY, value int)');
  await a.query('INSERT INTO session_test VALUES (1, 10), (2, 20)');
  await a.query('BEGIN'); await a.query('UPDATE session_test SET value = 11 WHERE id = 1');
  let finished = false;
  const blocked = b.query('UPDATE session_test SET value = value + 1 WHERE id = 1')
    .finally(() => {finished = true;});
  const pid = (await b.status()).pid;
  await waitLock(c, pid);
  assert(!finished && scalar(await c.query('SELECT value FROM session_test WHERE id = 2')) === '20', 'C progresses while B waits for A');
  assert((await a.status()).transaction === 2, 'A keeps its explicit transaction');
  await a.query('COMMIT'); await blocked;
  assert(scalar(await c.query('SELECT value FROM session_test WHERE id = 1')) === '12', 'A commit releases B');
  checks.push('kernel-lock-wait', 'third-connection-progress', 'commit-releases-waiter');

  await a.query("SELECT set_config('postgamma.session_marker', 'alpha', false)");
  assert(scalar(await b.query("SELECT current_setting('postgamma.session_marker', true)")) === null, 'GUC does not cross sessions');
  await a.query('CREATE TEMP TABLE private_value(value int)');
  await a.query('INSERT INTO private_value VALUES (101)');
  await b.query('CREATE TEMP TABLE private_value(value int)');
  await b.query('INSERT INTO private_value VALUES (202)');
  assert(scalar(await a.query('SELECT value FROM private_value')) === '101' &&
    scalar(await b.query('SELECT value FROM private_value')) === '202', 'Temporary namespaces are independent');
  await errorState(() => c.query('SELECT * FROM private_value'), '42P01');
  await a.query('PREPARE private_plan AS SELECT 101');
  await b.query('PREPARE private_plan AS SELECT 202');
  assert(scalar(await a.query('EXECUTE private_plan')) === '101' &&
    scalar(await b.query('EXECUTE private_plan')) === '202', 'Prepared state is independent');
  await errorState(() => c.query('EXECUTE private_plan'), '26000');
  await a.query('BEGIN ISOLATION LEVEL REPEATABLE READ');
  assert(scalar(await a.query('SELECT value FROM session_test WHERE id = 2')) === '20', 'Snapshot established');
  await b.query('UPDATE session_test SET value = 21 WHERE id = 2');
  assert(scalar(await a.query('SELECT value FROM session_test WHERE id = 2')) === '20' &&
    scalar(await c.query('SELECT value FROM session_test WHERE id = 2')) === '21', 'MVCC snapshots are independent');
  await a.query('COMMIT');
  const queued = await Promise.all([a.query('SELECT 1'), a.query('SELECT 2')]);
  assert(scalar(queued[0]) === '1' && scalar(queued[1]) === '2', 'One connection serializes ordinary queries');
  checks.push('guc-isolation', 'temp-isolation', 'prepared-isolation', 'snapshot-isolation', 'connection-serialization');
  await a.close(); await b.close(); await c.close();
}
export async function saturated(db: Database, checks: string[]) {
  const a = await db.connect('pool-holder'), b = await db.connect('pool-waiter'), c = await db.connect('pool-queued');
  await a.query('CREATE TABLE pool_test(id int PRIMARY KEY)');
  await a.query('INSERT INTO pool_test VALUES (1)');
  await a.query('BEGIN'); await a.query('SELECT * FROM pool_test WHERE id = 1 FOR UPDATE');
  await b.query('BEGIN');
  const occupied = await db.telemetry();
  assert(occupied.workers === 2 && occupied.pinned === 2n, 'Both small-pool carriers are pinned');
  const opening = db.connect('pool-new').then(connection => connection, error => error);
  // Admission at saturation may queue or return a capacity diagnostic. In
  // either case, the holder must retain a path to COMMIT and release the pool.
  let outcome: unknown;
  const waiting = c.query('SELECT 3').then(r => {outcome = scalar(r);}, e => {outcome = e;});
  let admission = await db.telemetry();
  const deadline = performance.now() + 1000;
  while (admission.requests === 0n && outcome === undefined && performance.now() < deadline)
    admission = await db.telemetry();
  assert(admission.pinned === 2n && (admission.requests > 0n || outcome !== undefined), 'Third request admitted at saturation');
  await a.query('COMMIT'); await b.query('ROLLBACK'); await waiting;
  const rejectedOpen = await opening;
  assert(rejectedOpen instanceof DatabaseError && rejectedOpen.status === 2 &&
    rejectedOpen.message.includes('carrier'), 'New connection cannot block the holder at saturation');
  assert(outcome === '3' || outcome instanceof DatabaseError &&
    (outcome.status === 2 || outcome.sqlstate === '53300'), 'Only success or capacity rejection is allowed');
  assert(scalar(await c.query('SELECT 4')) === '4', 'Pool progresses after holder commit');
  checks.push('small-pool-pinning', 'saturated-holder-progress');
  checks.push('connect-admission-at-saturation');
  await a.close(); await b.close(); await c.close();
}
