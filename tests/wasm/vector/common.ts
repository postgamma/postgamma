/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
import type {Connection} from '../src/api/database.js';
import {assert, scalar} from '../tests/helpers.js';
export const tables = ['postgamma_release_vector_hnsw', 'postgamma_release_vector_ivf'];
export const events = () => (window as any).probeEvents;
export async function fixtures() {
  const response = await fetch('./vector-fixtures.json');
  assert(response.ok, 'Vector fixtures are available'); return await response.json();
}
export async function seed(c: Connection, fixture: any, parallel = false) {
  await c.query('CREATE EXTENSION IF NOT EXISTS vector');
  await c.query("SET maintenance_work_mem = '16MB'");
  await c.query('SET max_parallel_maintenance_workers = ' + (parallel ? 1 : 0));
  if (parallel) {
    await c.query('SET max_parallel_workers = 1');
    await c.query('SET min_parallel_table_scan_size = 0');
    await c.query('SET log_min_messages = debug1');
  }
  for (const sql of fixture.nativeSetup) {
    if (parallel && sql.startsWith('CREATE INDEX')) {
      const table = sql.includes('_hnsw_idx') ? tables[0] : tables[1];
      await c.query(`ALTER TABLE ${table} SET (parallel_workers = 1)`);
    }
    await c.query(sql);
  }
}
export async function verify(c: Connection, label: string) {
  await c.query('SET enable_indexscan = off'); await c.query('SET enable_bitmapscan = off');
  await c.query('SET enable_seqscan = on');
  const exact = (await c.query(`SELECT id FROM ${tables[0]} ORDER BY embedding <-> '[1,1,1]', id LIMIT 10`))[0].rows.map(r => r[0]);
  assert(JSON.stringify(exact) === JSON.stringify(Array.from({length: 10}, (_, i) => String(i + 1))), 'Exact native fixture neighbors');
  await c.query('SET enable_indexscan = on'); await c.query('SET enable_seqscan = off');
  await c.query('SET hnsw.ef_search = 100'); await c.query('SET ivfflat.probes = 3');
  const observations = [];
  for (const table of tables) {
    const sql = `SELECT id, embedding <-> $1::vector AS distance FROM ${table} ORDER BY embedding <-> $1::vector LIMIT 10`;
    const parameters = [{typeOid: 0, value: '[1,1,1]'}];
    const plan = JSON.parse(scalar(await c.query('EXPLAIN (FORMAT JSON, COSTS OFF) ' + sql, {parameters})) as string);
    assert(JSON.stringify(plan).includes('"Index Name":"' + table + '_idx"'), 'Actual ANN index plan: ' + table);
    const rows = (await c.query(sql, {parameters}))[0].rows;
    const ids = rows.map(r => r[0]);
    assert(rows.length === 10 && new Set(ids).size === 10 && ids[0] === '1', 'ANN result cardinality, uniqueness and known nearest point');
    const recall = ids.filter(id => exact.includes(id)).length / exact.length;
    assert(recall >= 0.8, 'At least 8 of the exact top 10 on the fixed native dataset');
    let previous = -1;
    for (const [id, distance] of rows) {
      const i = Number(id), d = Number(distance);
      const expected = Math.hypot(i - 1, i % 17 - 1, i % 31 - 1);
      assert(i >= 1 && i <= 2000 && Math.abs(d - expected) < 1e-9 && d >= previous, 'ANN distances agree with stored vectors and ordered scan');
      previous = d;
    }
    observations.push({table, plan, ids, recall});
  }
  await c.query('RESET enable_seqscan'); await c.query('RESET enable_bitmapscan');
  events().push({kind: 'vector-neighbors', label, exact, observations});
  return observations;
}
export async function upstream(c: Connection, fixture: any) {
  for (const [index, statements] of Object.entries(fixture.upstream)) {
    for (const entry of statements as any[]) {
      const result = await c.query(entry.sql);
      if (entry.rows) assert(JSON.stringify(result[0].rows) === JSON.stringify(entry.rows), 'Pinned upstream result: ' + index + ': ' + entry.sql);
    }
  }
  await c.query('RESET enable_seqscan');
}
export async function transactions(a: Connection, b: Connection) {
  await a.query('BEGIN');
  for (const table of tables) await a.query(`INSERT INTO ${table} VALUES (3000, '[3000,8,24]')`);
  for (const table of tables) assert(scalar(await b.query(`SELECT count(*) FROM ${table} WHERE id = 3000`)) === '0', 'Peer cannot see uncommitted index rows');
  await a.query('ROLLBACK');
  for (const table of tables) assert(scalar(await b.query(`SELECT count(*) FROM ${table} WHERE id = 3000`)) === '0', 'Index insertion rollback');
  await a.query('BEGIN');
  await a.query('CREATE TABLE vector_rolled_back (v vector(3))');
  await a.query("INSERT INTO vector_rolled_back VALUES ('[1,2,3]')");
  await a.query('CREATE INDEX ON vector_rolled_back USING hnsw (v vector_l2_ops)');
  await a.query('ROLLBACK');
  assert(scalar(await b.query("SELECT to_regclass('vector_rolled_back')::text")) === null, 'Transactional extension DDL rollback');
  await a.query('BEGIN');
  for (const table of tables) await a.query(`INSERT INTO ${table} VALUES (3000, '[3000,8,24]')`);
  await a.query('COMMIT');
  for (const table of tables) assert(scalar(await b.query(`SELECT embedding::text FROM ${table} WHERE id = 3000`)) === '[3000,8,24]', 'Committed indexed vectors become visible');
}
