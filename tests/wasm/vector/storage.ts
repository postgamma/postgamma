/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
import {Database} from '../src/api/database.js';
import {assert, scalar} from '../tests/helpers.js';
import {Control} from '../storage-tests/control.js';
import {storageBudget} from '../storage-tests/budget.js';
import {fixtures, seed, verify, transactions, tables, events} from './common.js';

export async function vectorStorage(checks: string[], metrics: unknown[]) {
  const name = 'vectors-' + crypto.randomUUID(), data = await fixtures();
  let control: Control, db: Database;
  async function open(create: boolean) {
    const channel = crypto.randomUUID(); control = new Control(channel);
    db = await Database.open({create, storage: {type: 'opfs', name}, storageTestChannel: channel} as any);
    return await db.connect('persistent-vector');
  }
  async function close() {
    metrics.push(await db.close()); await control.wait(e => e.closed); control.close();
  }
  let c = await open(true);
  assert(scalar(await c.query('SHOW fsync')) === 'on' && scalar(await c.query('SHOW synchronous_commit')) === 'on', 'Vector storage retains WAL barriers');
  await seed(c, data);
  const peer = await db.connect('vector-peer');
  await transactions(c, peer); await peer.close();
  await verify(c, 'opfs-before-close'); await close();

  c = await open(false);
  assert(scalar(await c.query("SELECT extversion FROM pg_extension WHERE extname = 'vector'")) === '0.8.6', 'Extension catalog survives reopen');
  for (const table of tables) assert(scalar(await c.query(`SELECT embedding::text FROM ${table} WHERE id = 3000`)) === '[3000,8,24]', 'Committed indexed vector survives reopen');
  await verify(c, 'opfs-reopened');
  await c.query('BEGIN');
  for (const table of tables) await c.query(`INSERT INTO ${table} VALUES (4000, '[4000,5,1]')`);
  await control.arm('flush-before', '/pg_wal/', 'pause');
  let acknowledged = false;
  const pending = c.query('COMMIT').then(() => {acknowledged = true;}, error => String(error));
  const gate = await control.wait(e => e.gate === 'flush-before');
  assert(!acknowledged, 'Vector transaction is interrupted before WAL acknowledgement');
  events().push({kind: 'vector-interruption', gate, sample: db.sample(), method: 'Worker.terminate'});
  (db as any).transport.terminate();
  await pending; control.close();

  c = await open(false);
  const counts = [];
  for (const table of tables) {
    assert(scalar(await c.query(`SELECT embedding::text FROM ${table} WHERE id = 3000`)) === '[3000,8,24]', 'Acknowledged indexed vector survives crash');
    counts.push(scalar(await c.query(`SELECT count(*) FROM ${table} WHERE id = 4000`)));
    await c.query(`VACUUM ${table}`);
  }
  assert(counts[0] === counts[1] && ['0', '1'].includes(counts[0] as string), 'Unacknowledged cross-index transaction is atomic');
  await verify(c, 'opfs-crash-recovered-and-vacuumed');
  await close();
  const last = metrics[metrics.length - 1] as any;
  assert(last.output.some((line: string) => line.includes('automatic recovery in progress')), 'PostgreSQL WAL recovery actually ran with pgvector');
  events().push({kind: 'vector-storage-budget', budget: storageBudget(metrics, events()), pendingRows: counts});
  checks.push('opfs-vector-and-index-reopen', 'durable-vector-commit', 'worker-crash-index-recovery',
    'unacknowledged-cross-index-atomicity', 'index-vacuum-after-recovery', 'owned-storage-drain');
}
