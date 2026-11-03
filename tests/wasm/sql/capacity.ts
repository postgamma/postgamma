/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
import type {Connection, Database} from '../src/api/database.js';
import {limits} from '../src/protocol/limits.js';
import {assert, scalar, errorState} from './helpers.js';
const MiB = 1024 * 1024;
export async function capacityValues(c: Connection) {
  const bytes = limits.maximumValueBytes;
  const value = '中🐘'.repeat(Math.floor(bytes / 7)) + 'a'.repeat(bytes % 7);
  const roundtrip = await c.query('SELECT $1::text', {parameters: [{typeOid: 25, value}]});
  assert(scalar(roundtrip) === value, 'Exact maximum UTF-8 value survives parameter/result transfer');
  const binary = new Uint8Array(bytes);
  for (let i = 0; i < bytes; i++) binary[i] = i % 251;
  const rows = await c.query('SELECT $1::bytea', {parameters: [{typeOid: 17, value: binary}], binary: true});
  const observed = scalar(rows);
  assert(observed instanceof Uint8Array && observed.length === bytes && observed.every((v, i) => v === binary[i]), 'Exact maximum binary value owns every byte');
  await errorState(() => c.query(`SELECT repeat('x',${bytes + 1})`), '54000');
  const half = Math.floor(limits.resultBufferBytes / 2) + 512;
  await errorState(() => c.query(`SELECT repeat('x',${half}),repeat('y',${half})`), '54000');
  assert(scalar(await c.query('SELECT 16')) === '16', 'Connection recovers after value and result-buffer limits');
  let rejected = false;
  try {await c.query('SELECT 1 /*' + 'x'.repeat(limits.commandBytes) + '*/');}
  catch (error) {rejected = error instanceof Error && error.message.includes('query byte queue is full');}
  assert(rejected && scalar(await c.query('SELECT 17')) === '17', 'Oversized input is rejected before allocation and does not poison SQL');
}
export async function capacityStream(c: Connection) {
  const width = 64 * 1024, count = 128 * MiB / width;
  const sql = `SELECT g,repeat('s',${width}) FROM generate_series(1,${count}) g`;
  let rejected = false;
  try {await c.query(sql, {chunkRows: 8});}
  catch (error) {rejected = error instanceof Error && error.message.includes('Collected results exceed');}
  assert(rejected && scalar(await c.query('SELECT 18')) === '18', 'Collection cap cancels producer and permits reuse');
  const expected = 's'.repeat(width); let seen = 0, transferred = 0;
  for await (const batch of await c.execute(sql, {chunkRows: 8})) {
    for (const row of batch.rows) {
      assert(row[0] === String(++seen) && row[1] === expected, 'Large stream bytes and ordering'); transferred += width;
    }
  }
  assert(seen === count && transferred === 128 * MiB, 'Complete 128 MiB cumulative stream');
  const request = await c.execute(`SELECT g,repeat('c',${width}) FROM generate_series(1,1000000) g`, {chunkRows: 8});
  try {
    assert((await request.next())?.rows.length === 8, 'Large stream started before cancellation');
    await request.cancel();
    await errorState(async () => {for await (const _ of request) {}}, '57014');
  } finally {await request.close();}
  assert(scalar(await c.query('SELECT 19')) === '19', 'Connection reusable after explicit large-stream cancellation');
}
export async function capacity(db: Database, checks: string[]) {
  const connections: Connection[] = [];
  try {
    for (let i = 0; i < limits.maxConnections; i++) connections.push(await db.connect('capacity-' + i));
    const c = connections[0], observer = connections[1];
    await capacityValues(c);
    const paused = await c.execute("SELECT g,repeat('p',65536) FROM generate_series(1,1000000) g", {chunkRows: 8});
    try {
      const first = await paused.next(); assert(first?.rows.length === 8, 'Backpressure fixture starts');
      const before = db.sample()!.memory;
      await new Promise(resolve => setTimeout(resolve, 100));
      assert(scalar(await observer.query('SELECT 20')) === '20', 'Other connection progresses while large consumer pauses');
      assert(db.sample()!.memory - before <= 16 * MiB, 'Paused producer does not retain an unbounded result');
      assert(first.rows[0][1] === 'p'.repeat(65536), 'Retained result is stable during backpressure');
    } finally {await paused.close();}
    const value = 'a'.repeat(5 * MiB);
    const concurrent = await Promise.all(connections.map(c => c.query('SELECT pg_sleep(0.05),$1::text', {parameters: [{typeOid: 25, value}]})));
    assert(concurrent.every(result => result[0].rows[0][1] === value), 'Four concurrent 5 MiB roundtrips');
    await capacityStream(c);
    checks.push('maximum-value-text-binary', 'value-and-buffer-limit-reuse', 'large-input-admission',
      'large-stream-backpressure', 'four-large-roundtrips', 'stream-128mib', 'large-stream-cancel-reuse');
    (window as unknown as {probeEvents: unknown[]}).probeEvents.push({kind: 'sql-capacity-expanded',
      maximumValueBytes: limits.maximumValueBytes, streamedBytes: 128 * MiB, concurrentValueBytes: 5 * MiB, sample: db.sample()});
  } finally {await Promise.all(connections.map(c => c.close()));}
}
