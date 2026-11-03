/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
import {ClosedError, ConnectionBusyError} from '../src/api/database.js';
import type {Connection, Database, Transaction} from '../src/api/database.js';
import {assert, scalar, errorState} from './helpers.js';
const outcome = (work: Promise<unknown>) => work.then(() => undefined, error => error);
function deferred() {let resolve!: () => void; const promise = new Promise<void>(r => {resolve = r;}); return {promise, resolve};}

async function lockWait(c: Connection, peer: Connection) {
  const pid = (await c.status()).pid, deadline = performance.now() + 10000;
  while (performance.now() < deadline) {
    if (scalar(await peer.query("SELECT wait_event_type = 'Lock' FROM pg_stat_activity WHERE pid = $1::int4",
      {parameters: [{typeOid: 23, value: String(pid)}]})) === 't') return;
    await new Promise(resolve => setTimeout(resolve, 1));
  }
  throw new Error('Query never reached its expected lock wait');
}

export async function controls(db: Database, checks: string[]) {
  const c = await db.connect('control-owner'), peer = await db.connect('control-peer');
  await c.query('CREATE TABLE controlled_values(id int PRIMARY KEY)');
  await peer.exec('BEGIN; LOCK controlled_values IN ACCESS EXCLUSIVE MODE');
  const first = c.query('INSERT INTO controlled_values VALUES (1)');
  await lockWait(c, peer);
  const queuedAbort = new AbortController(), queuedReason = new Error('queued request canceled');
  const queued = outcome(c.query('INSERT INTO controlled_values VALUES (2)', {signal: queuedAbort.signal}));
  queuedAbort.abort(queuedReason);
  assert(await queued === queuedReason, 'Queued cancellation rejects with its original reason');
  await lockWait(c, peer); // The earlier request was not canceled.
  await peer.query('ROLLBACK'); await first;
  assert(scalar(await c.query('SELECT array_agg(id ORDER BY id)::text FROM controlled_values')) === '{1}',
    'Canceled queued SQL never reaches the database');
  checks.push('queued-abort-removes-command', 'queued-abort-preserves-earlier-request');

  await peer.exec('BEGIN; LOCK controlled_values IN ACCESS EXCLUSIVE MODE');
  const activeAbort = new AbortController(), activeReason = new Error('running request canceled');
  const active = outcome(c.query('INSERT INTO controlled_values VALUES (3)', {signal: activeAbort.signal}));
  await lockWait(c, peer); activeAbort.abort(activeReason);
  assert(await active === activeReason && (await db.telemetry()).requests === 0n,
    'Active cancellation retires the request before rejecting');
  await peer.query('ROLLBACK');
  assert(scalar(await c.query('SELECT count(*) FROM controlled_values')) === '1', 'Canceled write is rolled back');
  checks.push('active-abort-retires-before-rejection', 'active-abort-rollback-reuse');

  await peer.exec('BEGIN; LOCK controlled_values IN ACCESS EXCLUSIVE MODE');
  const timed = outcome(c.query('INSERT INTO controlled_values VALUES (4)', {timeoutMs: 1000}));
  await lockWait(c, peer);
  assert((await timed)?.name === 'TimeoutError', 'Deadline cancels an observed running query');
  const preparing = outcome(c.prepare('SELECT * FROM controlled_values', {timeoutMs: 1000}));
  await lockWait(c, peer);
  assert((await preparing)?.name === 'TimeoutError', 'Deadline also cancels Parse waiting for a relation lock');
  await peer.query('ROLLBACK');
  assert(scalar(await c.query('SELECT count(*) FROM pg_prepared_statements')) === '0', 'Canceled prepare releases its server statement');
  assert((await outcome(c.query('INSERT INTO controlled_values VALUES (5)', {timeoutMs: 0})))?.name === 'TimeoutError',
    'Expired deadline rejects before admission');
  checks.push('query-timeout', 'prepare-timeout-cleanup', 'expired-deadline');

  const streamingAbort = new AbortController();
  const request = await c.execute('SELECT generate_series(1, 1000000)', {chunkRows: 8, signal: streamingAbort.signal});
  assert((await request.next())?.rows.length === 8, 'Streaming query starts');
  streamingAbort.abort();
  assert((await outcome(request.next()))?.name === 'AbortError', 'Streaming next observes its abort');
  await request.close();
  assert(scalar(await c.query('SELECT 7')) === '7', 'Canceled stream leaves the connection reusable');
  checks.push('stream-abort-reuse');

  const entered = deferred(), release = deferred();
  const transaction = c.transaction(async tx => {
    await tx.query('INSERT INTO controlled_values VALUES (10)'); entered.resolve(); await release.promise;
    await tx.query('INSERT INTO controlled_values VALUES (11)'); return 'committed';
  }, {isolation: 'repeatable read'});
  await entered.promise;
  let outsideDone = false;
  const outside = c.query('INSERT INTO controlled_values VALUES (12)').then(() => {outsideDone = true;});
  assert(await outcome(c.execute('SELECT 1')) instanceof ConnectionBusyError, 'Direct execution cannot enter another transaction lease');
  assert(scalar(await peer.query('SELECT count(*) FROM controlled_values')) === '1' && !outsideDone,
    'Other sessions see no uncommitted writes and ordinary queries wait outside the callback');
  release.resolve(); assert(await transaction === 'committed', 'Callback returns its value after COMMIT'); await outside;
  assert(scalar(await c.query('SELECT array_agg(id ORDER BY id)::text FROM controlled_values')) === '{1,10,11,12}',
    'Queued outside query runs after the callback transaction');
  checks.push('transaction-exclusive-lease', 'transaction-commit-order');

  const sentinel = new Error('callback failed');
  assert(await outcome(c.transaction(async tx => {await tx.query('INSERT INTO controlled_values VALUES (20)'); throw sentinel;})) === sentinel,
    'Callback exception is preserved after rollback');
  await c.transaction(async tx => {
    await tx.query('SAVEPOINT recover');
    await errorState(() => tx.query('SELECT 1/0'), '22012');
    await tx.query('ROLLBACK TO SAVEPOINT recover');
    await tx.query('INSERT INTO controlled_values VALUES (21)');
  });
  assert(await outcome(c.transaction(async tx => {
    await tx.query('INSERT INTO controlled_values VALUES (22)');
    await outcome(tx.query('SELECT 1/0')); // Swallowing an SQL error does not turn ROLLBACK into success.
  })) instanceof Error, 'Failed transaction state cannot be reported as a successful commit');
  let retired!: Transaction;
  const timeout = outcome(c.transaction(async tx => {
    retired = tx; await tx.query('INSERT INTO controlled_values VALUES (23)'); await new Promise(() => {});
  }, {timeoutMs: 1000}));
  assert((await timeout)?.name === 'TimeoutError', 'Transaction deadline also interrupts an application callback');
  assert(await outcome(retired.query('SELECT 1')) instanceof ClosedError, 'A retired callback handle cannot affect later transactions');
  assert(scalar(await c.query('SELECT array_agg(id ORDER BY id)::text FROM controlled_values')) === '{1,10,11,12,21}',
    'Exception, failed-transaction and deadline cases all roll back their writes');
  assert((await db.telemetry()).requests === 0n, 'No native requests remain after control cases');
  checks.push('transaction-exception-rollback', 'transaction-savepoint-recovery', 'transaction-failed-state',
    'transaction-timeout-rollback', 'transaction-retired-handle');
  const closingEntered = deferred();
  const closingTransaction = outcome(c.transaction(async tx => {
    await tx.query('INSERT INTO controlled_values VALUES (24)'); closingEntered.resolve();
    await new Promise(() => {});
  }));
  await closingEntered.promise;
  await c.close();
  assert(await closingTransaction instanceof ClosedError, 'Connection close interrupts a waiting callback');
  assert(scalar(await peer.query('SELECT count(*) FROM controlled_values WHERE id = 24')) === '0',
    'Closing the connection rolls back callback writes');
  checks.push('transaction-close-rollback');
  await c.close(); await peer.close();
}
