/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
import type {Database} from '../src/api/database.js';
import {assert, scalar, errorState} from '../tests/helpers.js';
import {fixtures, seed, upstream, verify, transactions, events} from './common.js';

export async function vectorQueries(db: Database, checks: string[]) {
  const data = await fixtures(), connections = [];
  for (let i = 0; i < 4; i++) connections.push(await db.connect('vector-' + i));
  const [a, b] = connections;
  await a.query('CREATE EXTENSION vector');
  await seed(a, data, true);
  await verify(a, 'parallel-index-build');
  await transactions(a, b);
  await verify(a, 'transaction-rollback-and-commit');
  await a.query('SET max_parallel_maintenance_workers = 0');
  await upstream(a, data);
  await errorState(() => a.query("SELECT '[1,2]'::vector(3)"), '22000');
  assert(scalar(await a.query("SELECT vector_dims('[1,2,3]'::vector)")) === '3', 'Session recovers after vector input error');
  await Promise.all(connections.map(async (c, i) => {
    await c.query("SELECT vector_dims('[1,2,3]'::vector)");
    await c.query('SET hnsw.ef_search = ' + (20 + i));
    await c.query('SET ivfflat.probes = ' + (1 + i));
    await c.query('BEGIN');
  }));
  const work = connections.map(c => c.query("SELECT pg_sleep(0.3), current_setting('hnsw.ef_search'), current_setting('ivfflat.probes'), vector_dims('[1,2,3]'::vector)"));
  let live = await db.telemetry(); const deadline = performance.now() + 5000;
  while (live.running < 4n && performance.now() < deadline) live = await db.telemetry();
  assert(live.running === 4n, 'Four independent carriers execute with pgvector loaded');
  const results = await Promise.all(work);
  for (let i = 0; i < 4; i++) {
    assert(JSON.stringify(results[i][0].rows[0].slice(1)) === JSON.stringify([String(20 + i), String(1 + i), '3']), 'Independent custom GUC and extension role state');
    await connections[i].query('COMMIT');
  }
  for (let i = 0; i < 4; i++) assert(scalar(await connections[i].query("SELECT current_setting('hnsw.ef_search')")) === String(20 + i), 'Custom GUC survives request and transaction boundaries');
  events().push({kind: 'vector-concurrency', running: Number(live.running), sample: db.sample()});
  await Promise.all(connections.map(c => c.close()));
  checks.push('native-exact-and-ann-fixture', 'actual-hnsw-and-ivfflat-plans', 'parallel-index-builds',
    'transaction-and-ddl-rollback', 'pinned-upstream-l2-null-truncate', 'vector-error-reuse', 'four-carriers-custom-guc-isolation');
}
