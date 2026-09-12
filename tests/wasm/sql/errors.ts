/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
import {DatabaseError} from '../src/api/database.js';
import type {Database, Request, Result} from '../src/api/database.js';
import {assert, errorState, scalar} from './helpers.js';
import {waitLock} from './concurrency.js';
export async function collect(request: Request): Promise<Result[]> {
  const rows: Result[] = [];
  try {for (;;) {const batch = await request.next(); if (!batch) return rows; rows.push(batch);}}
  finally {await request.close();}
}
export async function errors(db: Database, checks: string[]) {
  const a = await db.connect('error-holder'), b = await db.connect('error-waiter'), c = await db.connect('error-observer');
  await a.query('CREATE TABLE error_test(id int PRIMARY KEY, value int)');
  await a.query('INSERT INTO error_test VALUES (1, 10), (2, 20)');
  await a.query('BEGIN'); await a.query('UPDATE error_test SET value = 11 WHERE id = 1');
  await b.query('BEGIN');
  const request = await b.execute('UPDATE error_test SET value = 12 WHERE id = 1');
  const canceled = collect(request).then(value => value, error => error);
  await waitLock(c, (await b.status()).pid);
  await request.cancel();
  const cancelError = await canceled;
  assert(cancelError instanceof DatabaseError && cancelError.sqlstate === '57014', 'Request cancellation SQLSTATE');
  assert((await b.status()).transaction === 3 && (await a.status()).transaction === 2, 'Cancellation is scoped to B');
  assert(scalar(await c.query('SELECT 100')) === '100', 'C remains usable after canceling B');
  await b.query('ROLLBACK'); await a.query('COMMIT');
  assert(scalar(await b.query('SELECT value FROM error_test WHERE id = 1')) === '11', 'Canceled update did not commit');
  checks.push('cancel-real-lock-wait', 'cancel-request-isolation', 'rollback-after-cancel');

  await b.query("SET statement_timeout = '100ms'"); await b.query('BEGIN');
  const timeout = await errorState(() => b.query('SELECT pg_sleep(2)'), '57014');
  assert(timeout.message.includes('statement timeout') && (await b.status()).transaction === 3, 'Statement timeout originates in PostgreSQL');
  await b.query('ROLLBACK'); await b.query('SET statement_timeout = 0');
  await a.query('BEGIN'); await a.query('UPDATE error_test SET value = 13 WHERE id = 1');
  await b.query("SET lock_timeout = '100ms'"); await b.query('BEGIN');
  const lockTimeout = await errorState(() => b.query('UPDATE error_test SET value = 14 WHERE id = 1'), '55P03');
  assert(lockTimeout.message.includes('lock timeout') && (await b.status()).transaction === 3, 'Lock timeout originates in PostgreSQL');
  await b.query('ROLLBACK'); await a.query('ROLLBACK'); await b.query('SET lock_timeout = 0');
  checks.push('statement-timeout-57014', 'lock-timeout-55P03', 'rollback-after-timeouts');

  for (const connection of [a, b]) {
    await connection.query('SET statement_timeout = 0');
    await connection.query('SET lock_timeout = 0');
    await connection.query("SET deadlock_timeout = '100ms'");
    await connection.query('BEGIN');
  }
  // Both first row locks are acquired before either second update is sent.
  await Promise.all([a.query('UPDATE error_test SET value = value + 1 WHERE id = 1'),
    b.query('UPDATE error_test SET value = value + 1 WHERE id = 2')]);
  const pids = [(await a.status()).pid, (await b.status()).pid];
  assert(scalar(await c.query(`SELECT count(DISTINCT pid) FROM pg_locks WHERE pid IN (${pids.join(',')})
    AND locktype = 'transactionid' AND mode = 'ExclusiveLock' AND granted`)) === '2', 'First-lock barrier has two live transaction owners');
  const crossed = await Promise.allSettled([
    a.query('UPDATE error_test SET value = value + 1 WHERE id = 2'),
    b.query('UPDATE error_test SET value = value + 1 WHERE id = 1'),
  ]);
  const victims = crossed.filter(value => value.status === 'rejected') as PromiseRejectedResult[];
  assert(victims.length === 1 && victims[0].reason instanceof DatabaseError &&
    victims[0].reason.sqlstate === '40P01', 'PostgreSQL selects exactly one deadlock victim');
  const states = [(await a.status()).transaction, (await b.status()).transaction];
  assert(states.filter(state => state === 3).length === 1 && states.filter(state => state === 2).length === 1,
    'Victim aborts and survivor retains its transaction');
  const detail = victims[0].reason.diagnostics[3];
  assert(typeof detail === 'string' && detail.includes('waits for'), 'Real deadlock wait graph diagnostic');
  (window as unknown as {probeEvents: unknown[]}).probeEvents.push({kind: 'deadlock', pids, states,
    sqlstate: victims[0].reason.sqlstate, detail});
  await a.query('ROLLBACK'); await b.query('ROLLBACK');
  assert(scalar(await a.query('SELECT 1')) === '1' && scalar(await b.query('SELECT 2')) === '2', 'Both sessions are reusable after deadlock');
  checks.push('crossed-row-deadlock-40P01', 'deadlock-diagnostic', 'rollback-after-deadlock');

  for (let round = 0; round < 8; round++) {
    const racing = await b.execute('SELECT 9');
    const result = collect(racing).then(value => value, error => error);
    await racing.cancel();
    const outcome = await result;
    assert(Array.isArray(outcome) && scalar(outcome) === '9' ||
      outcome instanceof DatabaseError && outcome.sqlstate === '57014', 'Completion/cancel race preserves one terminal outcome');
    assert(scalar(await b.query('SELECT 10')) === '10', 'Late cancel does not affect the next request');
  }
  checks.push('completion-cancel-race');
  await a.close(); await b.close(); await c.close();
}
