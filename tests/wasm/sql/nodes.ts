/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
import type {Connection, Database} from '../src/api/database.js';
import {assert, scalar} from './helpers.js';

// Exercise stored parse trees in fresh sessions. Initdb alone caught outNode
// and nodeRead overflowing in Safari 16.6, but not the other node operations.
export async function nodeRecursion(db: Database, checks: string[]) {
  let c = await db.connect();
  let peer: Connection | undefined, passed = false;
  const depth = 32;
  const expression = (value: string) => '('.repeat(depth) + value + ' + 1)'.repeat(depth);
  try {
    await c.exec(`DROP SCHEMA IF EXISTS wasm_node_recursion CASCADE;
      CREATE SCHEMA wasm_node_recursion;
      CREATE TABLE wasm_node_recursion.items(n integer);
      INSERT INTO wasm_node_recursion.items SELECT generate_series(1, 5);
      INSERT INTO wasm_node_recursion.items VALUES (NULL);
      CREATE FUNCTION wasm_node_recursion.add_depth(x integer) RETURNS integer
        LANGUAGE sql IMMUTABLE RETURN ${expression('x')};
      CREATE VIEW wasm_node_recursion.values AS
        SELECT n, ${expression('n')} AS value FROM wasm_node_recursion.items;`);
    await c.close();
    c = await db.connect();
    await c.exec('SET compute_query_id = on');
    assert(scalar(await c.query(`SELECT sum(value) FROM wasm_node_recursion.values`)) === '175',
      'Stored nested view tree survives session reopen');
    assert(scalar(await c.query(`SELECT sum(wasm_node_recursion.add_depth(n))
      FROM wasm_node_recursion.items`)) === '175', 'Stored SQL function tree survives session reopen');
    assert(scalar(await c.query(`SELECT count(*) FROM wasm_node_recursion.values
      WHERE value IS NULL`)) === '1', 'Nested stored expression preserves NULL');
    const plan = JSON.parse(String(scalar(await c.query(`EXPLAIN (VERBOSE, FORMAT JSON)
      SELECT value, count(*) FROM wasm_node_recursion.values GROUP BY value ORDER BY value`))));
    assert(plan[0]['Query Identifier'] !== undefined, 'Query jumbling remains active');
    checks.push('node-stored-view', 'node-stored-sql-function', 'node-null-semantics', 'node-query-jumbling');
    peer = await db.connect();
    await peer.exec(`CREATE OR REPLACE VIEW wasm_node_recursion.values AS
      SELECT n, ${expression('n')} + 1 AS value FROM wasm_node_recursion.items`);
    assert(scalar(await c.query('SELECT sum(value) FROM wasm_node_recursion.values')) === '180',
      'Relation invalidation reloads the changed stored tree');
    checks.push('node-relation-invalidation');
    passed = true;
  } finally {
    const cleanup = async () => {
      try {await c.exec('RESET compute_query_id; DROP SCHEMA IF EXISTS wasm_node_recursion CASCADE');}
      finally {try {await peer?.close();} finally {await c.close();}}
    };
    // A fatal worker error also rejects cleanup requests. Preserve the original
    // Wasm stack in that case instead of replacing it with a ClosedError.
    if (passed) await cleanup(); else await cleanup().catch(() => {});
  }
}
