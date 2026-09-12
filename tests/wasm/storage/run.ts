/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
import {Database} from '../src/api/database.js';
import {assert, scalar} from '../tests/helpers.js';
import {Control} from './control.js';
export async function storage(checks: string[], metrics: unknown[]) {
  const name = 'storage-' + crypto.randomUUID();
  const channel = 'storage-test-' + crypto.randomUUID(), control = new Control(channel);
  const options = {storage: {type: 'opfs' as const, name}, storageTestChannel: channel};
  const db = await Database.open(options);
  const a = await db.connect('storage-a'), b = await db.connect('storage-b');
  await a.query('CREATE TABLE durable_values (id int PRIMARY KEY, value text)');
  await a.query("INSERT INTO durable_values VALUES (1, 'committed')");
  await a.query('BEGIN'); await a.query("UPDATE durable_values SET value = 'uncommitted' WHERE id = 1");
  assert(scalar(await b.query('SELECT value FROM durable_values')) === 'committed', 'OPFS sessions remain independent');
  await a.query('ROLLBACK');
  let denied = false;
  try {const other = await Database.open({...options, create: false}); await other.close();}
  catch (error) {denied = String(error).includes('live owner');}
  assert(denied, 'Second owner is rejected before initdb/recovery');
  const probe = await (db as any).transport.call(65534, undefined, (w: any) => w.u32(1));
  assert(probe.u32() >= 20, 'OPFS file identity, descriptor and namespace semantics'); probe.end();
  const storageMetrics = await control.metrics();
  assert(storageMetrics.flushes > 0 && storageMetrics.directoryFlushes > 0 && storageMetrics.peakHandles <= 64,
    'Data and namespace flushes reach OPFS within the handle bound');
  await control.arm('write', '/pending-write');
  const io = (db as any).transport.call(65534, undefined, (w: any) => w.u32(3));
  await control.wait(event => event.gate === 'write');
  let closed = false;
  const close = db.close().then(value => {closed = true; return value;});
  await new Promise(resolve => setTimeout(resolve, 50));
  assert(!closed, 'Close waits for in-flight storage before releasing resources');
  denied = false;
  try {const other = await Database.open({...options, create: false}); await other.close();}
  catch (error) {denied = String(error).includes('live owner');}
  assert(denied, 'Ownership remains held during in-flight close');
  control.release();
  const completed = await io; assert(completed.u32() === 4, 'Peer close drains the actual write'); completed.end();
  metrics.push(await close); await control.wait(event => event.closed); control.close();
  const reopened = await Database.open({...options, create: false});
  const c = await reopened.connect();
  assert(scalar(await c.query('SELECT value FROM durable_values')) === 'committed', 'Reopen persists SQL and rollback');
  const again = await (reopened as any).transport.call(65534, undefined, (w: any) => w.u32(2));
  assert(again.u32() === 3, 'Reopen retains renamed directory/file'); again.end();
  metrics.push(await reopened.close());
  checks.push('owned-before-startup', 'second-owner-rejected', 'independent-opfs-sessions',
    'open-file-rename-unlink', 'dup-and-positioned-io', 'directory-rename', 'readonly-truncate-sync',
    'close-drains-inflight-io', 'ownership-held-during-close', 'healthy-storage-close', 'sql-and-namespace-reopen');
}
