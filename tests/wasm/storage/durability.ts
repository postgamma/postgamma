/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
import {Database} from '../src/api/database.js';
import {assert, scalar, errorState} from '../tests/helpers.js';
import {waitLock} from '../tests/concurrency.js';
import {Control} from './control.js';
import {legacyFixture} from './migration.js';

async function probe(db: Database, phase: number) {
  const reply = await (db as any).transport.call(65534, undefined, (w: any) => w.u32(phase));
  assert(reply.u32() > 0, 'Filesystem fault probe ' + phase); reply.end();
}
export async function durability(checks: string[], metrics: unknown[]) {
  const name = 'durability-' + crypto.randomUUID(), channel = crypto.randomUUID();
  const control = new Control(channel);
  const options = {storage: {type: 'opfs' as const, name}, storageTestChannel: channel};
  let db = await Database.open(options);
  const a = await db.connect('wal-holder'), b = await db.connect('wal-waiter'), c = await db.connect('wal-observer');
  const fourth = await db.connect('storage-capacity');
  const running = [a, b, c, fourth].map(connection => connection.query('SELECT pg_sleep(0.3)'));
  const deadline = performance.now() + 5000;
  let telemetry = await db.telemetry();
  while (telemetry.running < 4n && performance.now() < deadline) telemetry = await db.telemetry();
  assert(telemetry.running === 4n, 'Four independent carriers run with the I/O owner present');
  await Promise.all(running);
  checks.push('four-opfs-carriers');
  assert(scalar(await a.query('SHOW fsync')) === 'on' && scalar(await a.query('SHOW synchronous_commit')) === 'on',
    'PostgreSQL durability settings remain enabled');
  await a.query('CREATE TABLE durable_values(id int PRIMARY KEY, value int)');
  await a.query('INSERT INTO durable_values VALUES(1, 10)');
  await a.query('BEGIN'); await a.query('UPDATE durable_values SET value = 11 WHERE id = 1');
  let waiterDone = false, commitDone = false;
  const waiter = b.query('UPDATE durable_values SET value = value + 1 WHERE id = 1').finally(() => {waiterDone = true;});
  await waitLock(c, (await b.status()).pid);
  await control.arm('flush-before', '/pg_wal/');
  const commit = a.query('COMMIT').finally(() => {commitDone = true;});
  await control.wait(event => event.gate === 'flush-before');
  await new Promise(resolve => setTimeout(resolve, 50));
  assert(!commitDone && !waiterDone, 'Synchronous COMMIT and lock release wait for actual WAL flush');
  assert(scalar(await c.query('SELECT 17')) === '17', 'Unrelated carrier and control Worker progress during blocked I/O');
  control.release(); await commit; await waiter;
  assert(scalar(await c.query('SELECT value FROM durable_values')) === '12', 'Flush completes commit and releases waiter');
  checks.push('wal-flush-before-ack', 'wal-flush-before-lock-release', 'io-owner-independent-progress');

  for (const [phase, operation, path, action] of [
    [4, 'write', '/faults/data', 'short'], [5, 'create', '/faults/quota', 'quota'],
    [6, 'flush-before', '/faults/data', 'error'], [7, 'flush-before', '/faults/data', 'error'],
    [8, 'truncate', '/faults/data', 'error'], [9, 'directory-flush', '/faults', 'error'],
  ] as const) {
    await control.arm(operation, path, action); await probe(db, phase);
    await control.wait(event => event.gate === operation);
  }
  await control.arm('write', '/faults/copy-out', 'quota');
  await errorState(() => a.query("COPY (SELECT repeat('x',8192)) TO '/persistent/faults/copy-out'"), '53100');
  assert(scalar(await a.query('SELECT 19')) === '19', 'Session reuses after SQL quota error');
  checks.push('short-write-count', 'create-enospc', 'fsync-eio', 'fdatasync-eio', 'truncate-eio', 'directory-fsync-eio',
    'sql-quota-sqlstate-and-reuse');
  const previous = await control.metrics();
  const compacted = await control.checkpoint();
  assert(compacted.compactions === previous.compactions + 1, 'An actual namespace checkpoint completed');
  assert(scalar(await a.query('SELECT value FROM durable_values')) === '12', 'Live descriptors survive namespace compaction');
  checks.push('namespace-checkpoint', 'open-files-after-checkpoint');
  metrics.push(await db.close());
  const closed = await control.wait(event => event.closed);
  assert(closed.handles === 0 && closed.virtualOpens === 0 && !closed.failed, 'Storage drains all handles');
  (window as any).probeEvents.push({kind: 'storage-budget', ...closed});

  // A missing journal must preserve data for diagnosis, never recycle it as orphans.
  const root = await (await (await navigator.storage.getDirectory()).getDirectoryHandle('postgamma')).getDirectoryHandle(name);
  await legacyFixture(root);
  db = await Database.open({...options, create: false});
  const migrated = await db.connect();
  assert(scalar(await migrated.query('SELECT value FROM durable_values')) === '12', 'Legacy namespace migration preserves PostgreSQL files');
  metrics.push(await db.close());
  await control.wait(event => event.closed);
  checks.push('legacy-namespace-migration');
  const journal = await (await root.getFileHandle('namespace-v1')).getFile();
  const backup = await journal.arrayBuffer();
  const entries = async () => {const names: string[] = []; for await (const key of (root as any).keys()) names.push(key); return names.sort();};
  await root.removeEntry('namespace-v1'); const before = await entries();
  let rejected = false;
  try {db = await Database.open({...options, create: false}); await db.close();} catch {rejected = true;}
  assert(rejected && JSON.stringify(await entries()) === JSON.stringify(before), 'Missing journal rejects open without data deletion');
  const writer = await (await root.getFileHandle('namespace-v1', {create: true})).createWritable();
  await writer.write(backup); await writer.close();
  db = await Database.open({...options, create: false});
  const d = await db.connect();
  assert(scalar(await d.query('SELECT value FROM durable_values')) === '12', 'Acknowledged transactions survive fresh mount');
  await control.arm('close-flush', '/', 'error');
  rejected = false;
  try {await db.close();} catch {rejected = true;}
  const failedClose = await control.wait(event => event.closed);
  assert(rejected && failedClose.failed && failedClose.handles === 0, 'Flush failure rejects close and releases physical handles');
  db = await Database.open({...options, create: false});
  const e = await db.connect();
  assert(scalar(await e.query('SELECT value FROM durable_values')) === '12', 'Fresh instance opens after teardown failure');
  metrics.push(await db.close()); control.close();
  checks.push('missing-journal-preserves-data', 'close-flush-failure', 'reopen-after-close-failure');
}
