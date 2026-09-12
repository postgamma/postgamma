/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
import {DatabaseError} from '../src/api/database.js';
import type {Database} from '../src/api/database.js';
import {limits} from '../src/protocol/limits.js';
import {assert, scalar} from './helpers.js';
export async function budget(db: Database, checks: string[]) {
  const connections = [];
  for (let i = 0; i < limits.maxConnections; i++) connections.push(await db.connect(`budget-${i}`));
  let capacity = false;
  try {await db.connect('over-budget');} catch (error) {capacity = error instanceof DatabaseError && error.status === 2;}
  assert(capacity, 'Live connection admission enforces the measured profile');
  const work = connections.map((c, i) => c.query(`SELECT pg_sleep(0.3), ${i}`));
  let live = await db.telemetry();
  const deadline = performance.now() + 5000;
  while (live.running < BigInt(limits.maxConnections) && performance.now() < deadline) live = await db.telemetry();
  assert(live.running === BigInt(limits.maxConnections), 'All admitted sessions run on independent carriers');
  await Promise.all(work);
  const c = connections[0], payload = new Uint8Array(600 * 1024);
  const queued = await Promise.allSettled(Array.from({length: 4}, () => c.query('SELECT octet_length($1::bytea)', {
    parameters: [{typeOid: 17, value: payload}],
  })));
  assert(queued.filter(value => value.status === 'fulfilled').length === 3, 'Bounded queued parameter bytes');
  const rejected = queued.filter(value => value.status === 'rejected') as PromiseRejectedResult[];
  assert(rejected.length === 1 && rejected[0].reason.message.includes('query byte queue is full'), 'Queue capacity is observable');
  assert(scalar(await c.query('SELECT 9')) === '9', 'Queue saturation does not poison a session');
  checks.push('max-live-connections', 'four-running-carriers', 'bounded-queued-parameters', 'reuse-after-queue-rejection');
  (window as unknown as {probeEvents: unknown[]}).probeEvents.push({kind: 'sql-capacity', connections: limits.maxConnections,
    running: Number(live.running), sample: db.sample()});
  await Promise.all(connections.map(connection => connection.close()));
}
