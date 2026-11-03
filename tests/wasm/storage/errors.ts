/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
import {Database} from '../src/api/database.js';
import {assert, scalar} from '../tests/helpers.js';
import {Control} from './control.js';

export async function storageErrors(checks: string[], metrics: unknown[]) {
  const name = 'storage-errors-' + crypto.randomUUID(), channel = crypto.randomUUID();
  const control = new Control(channel);
  const options = {storage: {type: 'opfs' as const, name}, storageTestChannel: channel};
  let db = await Database.open(options), c = await db.connect();
  await c.query('CREATE TABLE durable_values(id int PRIMARY KEY)');
  await c.query('INSERT INTO durable_values VALUES(1)');
  await c.query('BEGIN'); await c.query('INSERT INTO durable_values VALUES(2),(3)');
  await control.arm('flush-before', '/pg_wal/', 'error');
  let rejected = false;
  try {await c.query('COMMIT');} catch {rejected = true;}
  assert(rejected, 'WAL flush error cannot acknowledge COMMIT');
  // Discard through the control Worker and await physical storage release.
  await (db as any).transport.fail(new Error('Discard failed storage instance'));
  db = await Database.open({...options, create: false}); c = await db.connect();
  assert(scalar(await c.query('SELECT count(*) FROM durable_values WHERE id = 1')) === '1', 'Prior acknowledged commit remains');
  const count = scalar(await c.query('SELECT count(*) FROM durable_values WHERE id IN (2,3)'));
  assert(count === '0' || count === '2', 'Unacknowledged transaction remains atomic');
  checks.push('wal-flush-failure-rejects-commit', 'recovery-preserves-ack', 'unacknowledged-transaction-atomic');
  // Hold real browser I/O inside a WAL barrier while retiring the transport.
  // Cleanup must unblock that work and close its handles without calling back
  // into native Completion objects belonging to the discarded kernel.
  await c.query('BEGIN'); await c.query('INSERT INTO durable_values VALUES(4),(5)');
  await control.arm('flush-before', '/pg_wal/', 'pause');
  const pendingCommit = c.query('COMMIT').then(() => null, error => error);
  await control.wait(event => event.gate === 'flush-before' && event.action === 'pause');
  await (db as any).transport.fail(new Error('Discard while a WAL barrier is pending'));
  assert(await pendingCommit instanceof Error, 'Fatal retirement rejects the paused COMMIT');
  const retired = db.diagnostics() as {storageQuiesced: boolean; storageReleased: boolean};
  assert(retired.storageQuiesced && retired.storageReleased, 'Paused I/O quiesces and closes physical handles before rejection');
  (window as any).probeEvents.push({kind: 'storage-fatal', metrics: retired});
  db = await Database.open({...options, create: false}); c = await db.connect();
  assert(scalar(await c.query('SELECT count(*) FROM durable_values WHERE id = 1')) === '1', 'Acknowledged data survives interrupted I/O');
  const pausedCount = scalar(await c.query('SELECT count(*) FROM durable_values WHERE id IN (4,5)'));
  assert(pausedCount === '0' || pausedCount === '2', 'Interrupted WAL transaction remains atomic');
  checks.push('fatal-drains-paused-wal', 'fatal-physical-handle-release');
  await control.arm('write', '/faults/data', 'short');
  let reply = await (db as any).transport.call(65534, undefined, (w: any) => w.u32(4)); reply.u32(); reply.end();
  await control.arm('namespace-written', 'move', 'error');
  reply = await (db as any).transport.call(65534, undefined, (w: any) => w.u32(10));
  assert(reply.u32() === 1, 'Uncertain namespace append returns EIO'); reply.end();
  await (db as any).transport.fail(new Error('Discard poisoned namespace'));
  db = await Database.open({...options, create: false}); c = await db.connect();
  reply = await (db as any).transport.call(65534, undefined, (w: any) => w.u32(11));
  assert(reply.u32() === 2, 'Namespace replays atomic rename with original file contents'); reply.end();
  assert(scalar(await c.query('SELECT count(*) FROM durable_values WHERE id = 1')) === '1', 'SQL persists across namespace failure');
  metrics.push(await db.close()); await control.wait(event => event.closed); control.close();
  checks.push('namespace-error-propagates', 'namespace-recovery-retains-identity');
}
