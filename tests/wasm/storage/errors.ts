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
  // A PANIC retires the entire module; the browser owns final access-handle release.
  (db as any).transport.fail(new Error('Discard failed storage instance'));
  db = await Database.open({...options, create: false}); c = await db.connect();
  assert(scalar(await c.query('SELECT count(*) FROM durable_values WHERE id = 1')) === '1', 'Prior acknowledged commit remains');
  const count = scalar(await c.query('SELECT count(*) FROM durable_values WHERE id IN (2,3)'));
  assert(count === '0' || count === '2', 'Unacknowledged transaction remains atomic');
  checks.push('wal-flush-failure-rejects-commit', 'recovery-preserves-ack', 'unacknowledged-transaction-atomic');
  await control.arm('write', '/faults/data', 'short');
  let reply = await (db as any).transport.call(65534, undefined, (w: any) => w.u32(4)); reply.u32(); reply.end();
  await control.arm('namespace-written', 'move', 'error');
  reply = await (db as any).transport.call(65534, undefined, (w: any) => w.u32(10));
  assert(reply.u32() === 1, 'Uncertain namespace append returns EIO'); reply.end();
  (db as any).transport.fail(new Error('Discard poisoned namespace'));
  db = await Database.open({...options, create: false}); c = await db.connect();
  reply = await (db as any).transport.call(65534, undefined, (w: any) => w.u32(11));
  assert(reply.u32() === 2, 'Namespace replays atomic rename with original file contents'); reply.end();
  assert(scalar(await c.query('SELECT count(*) FROM durable_values WHERE id = 1')) === '1', 'SQL persists across namespace failure');
  metrics.push(await db.close()); await control.wait(event => event.closed); control.close();
  checks.push('namespace-error-propagates', 'namespace-recovery-retains-identity');
}
