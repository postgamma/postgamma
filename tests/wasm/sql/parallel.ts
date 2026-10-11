/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
import {Database, DatabaseError} from '../src/api/database.js';
import type {Connection} from '../src/api/database.js';
import {assert, errorState, scalar} from './helpers.js';
import {collect} from './errors.js';
import {pthreadPoolSize} from '../src/protocol/limits.js';

const events = () => (window as unknown as {probeEvents: unknown[]}).probeEvents;
const WORKERS = 2;
function nodes(plan: any): any[] {return [plan, ...(plan.Plans || []).flatMap(nodes)];}
async function enable(c: Connection) {
  await c.exec(`SET max_parallel_workers = ${WORKERS}; SET max_parallel_workers_per_gather = ${WORKERS};
    SET min_parallel_table_scan_size = 0; SET min_parallel_index_scan_size = 0;
    SET parallel_setup_cost = 0; SET parallel_tuple_cost = 0;
    SET parallel_leader_participation = on; SET debug_parallel_query = off; SET enable_indexscan = off;
    SET enable_bitmapscan = off; SET enable_mergejoin = off; SET enable_nestloop = off`);
}
async function explain(c: Connection, sql: string, label: string, required: (rows: any[]) => boolean) {
  const plan = JSON.parse(scalar(await c.query('EXPLAIN (ANALYZE, VERBOSE, FORMAT JSON, TIMING OFF) ' + sql)) as string)[0].Plan;
  const rows = nodes(plan), gathers = rows.filter(n => n['Node Type'].startsWith('Gather'));
  events().push({kind: 'parallel-plan', label, plan});
  assert(gathers.length > 0 && gathers.every(n => n['Workers Planned'] === WORKERS && n['Workers Launched'] === WORKERS),
    label + ': EXPLAIN ANALYZE must show two planned and two launched workers');
  assert(required(rows), label + ': expected parallel plan');
}
async function equal(c: Connection, sql: string, label: string, required: (rows: any[]) => boolean) {
  await c.query('SET max_parallel_workers_per_gather = 0');
  const serial = await c.query(sql);
  await c.query('SET max_parallel_workers_per_gather = ' + WORKERS);
  await explain(c, sql, label, required);
  const parallel = await c.query(sql);
  assert(JSON.stringify(parallel.map(r => ({columns: r.columns, rows: r.rows}))) ===
    JSON.stringify(serial.map(r => ({columns: r.columns, rows: r.rows}))), label + ': complete ordered results equal serial execution');
}
async function workerState(observer: Connection, pid: number, expected: number) {
  const deadline = performance.now() + 15000;
  while (performance.now() < deadline) {
    const rows = (await observer.query(`SELECT pid, wait_event FROM pg_stat_activity
      WHERE backend_type = 'parallel worker' AND leader_pid = ${pid}`))[0].rows;
    if (rows.length === expected) {
      events().push({kind: 'parallel-worker-state', leader: pid, expected, rows}); return;
    }
    await new Promise(resolve => setTimeout(resolve, 10));
  }
  throw new Error('Parallel worker count did not reach ' + expected);
}
async function queries(db: Database, checks: string[], label: string) {
  const a = await db.connect('parallel-query'), b = await db.connect('parallel-observer');
  await a.exec(`CREATE TABLE parallel_fact AS SELECT i AS id, i % 97 AS g,
    CASE WHEN i % 11 = 0 THEN NULL ELSE i::bigint END AS v FROM generate_series(1, 20000) i;
    ALTER TABLE parallel_fact SET (parallel_workers = ${WORKERS}); ANALYZE parallel_fact`);
  await enable(a);
  const scan = (ns: any[]) => ns.some(n => n['Node Type'] === 'Seq Scan' && n['Parallel Aware']);
  const aggregate = (ns: any[]) => scan(ns) && ns.some(n => n['Node Type'] === 'Aggregate' && n['Partial Mode'] === 'Partial');
  await equal(a, 'SELECT id, g, v FROM parallel_fact WHERE id % 71 = 0 ORDER BY id', label + '-scan', scan);
  await equal(a, 'SELECT g, count(*), count(v), sum(v), min(v), max(v) FROM parallel_fact GROUP BY g ORDER BY g', label + '-aggregate', aggregate);
  await equal(a, `SELECT a.g, count(*), sum(a.v + b.v) FROM parallel_fact a
    JOIN parallel_fact b ON a.id = b.id WHERE a.id % 3 = 0 GROUP BY a.g ORDER BY a.g`,
    label + '-hash-join', ns => scan(ns) && ns.some(n => n['Node Type'].startsWith('Gather') &&
      nodes(n).some(child => child['Node Type'] === 'Hash Join')));
  await equal(a, 'SELECT id, g, v FROM parallel_fact ORDER BY g, id LIMIT 257', label + '-gather-merge',
    ns => scan(ns) && ns.some(n => n['Node Type'] === 'Gather Merge'));
  checks.push('parallel-two-workers', 'parallel-scan-values', 'parallel-aggregate-values', 'parallel-hash-join-values', 'parallel-gather-merge-values');

  await a.query("SET postgamma.parallel_marker = 'query-owner'");
  await b.query("SET postgamma.parallel_marker = 'observer'");
  const guc = "SELECT count(*) FILTER (WHERE current_setting('postgamma.parallel_marker') = 'query-owner') FROM parallel_fact";
  await explain(a, guc, label + '-worker-guc', aggregate);
  assert(scalar(await a.query(guc)) === '20000', 'Worker receives the originating session GUC');
  assert(scalar(await b.query("SHOW postgamma.parallel_marker")) === 'observer', 'Parallel GUC restore preserves other sessions');
  checks.push('parallel-guc-isolation');

  await a.query('BEGIN ISOLATION LEVEL REPEATABLE READ');
  const count = 'SELECT count(*) FROM parallel_fact';
  await explain(a, count, label + '-snapshot', aggregate);
  assert(scalar(await a.query(count)) === '20000', 'Initial parallel snapshot');
  await b.query('INSERT INTO parallel_fact VALUES (20001, 1, 20001)');
  assert(scalar(await a.query(count)) === '20000', 'Workers retain the leader transaction snapshot');
  await a.query('COMMIT');
  assert(scalar(await a.query(count)) === '20001', 'Next transaction sees the committed row');
  checks.push('parallel-snapshot');

  await a.exec(`CREATE FUNCTION parallel_pause(x int) RETURNS int LANGUAGE plpgsql STABLE PARALLEL SAFE AS $$
    BEGIN PERFORM pg_sleep(0.001); RETURN x; END $$;
    CREATE FUNCTION parallel_fail(x int) RETURNS int LANGUAGE plpgsql STABLE PARALLEL SAFE AS $$
    BEGIN RAISE EXCEPTION 'parallel worker test failure' USING ERRCODE = '22012'; END $$`);
  const pid = (await a.status()).pid;
  const slow = 'SELECT sum(parallel_pause(id)) FROM parallel_fact';
  // Verify both workers also execute when the leader only coordinates results.
  // Keep errors and cancellation in the workers rather than the leader.
  await a.query('SET parallel_leader_participation = off');
  await explain(a, count, label + '-leader-excluded', aggregate);
  checks.push('parallel-leader-excluded');
  await a.query('BEGIN');
  const request = await a.execute(slow), pending = collect(request).then(value => value, error => error);
  await workerState(b, pid, WORKERS);
  assert(scalar(await b.query('SELECT 42')) === '42', 'Other sessions progress during parallel execution');
  const c = await db.connect('parallel-peer-c'), d = await db.connect('parallel-peer-d');
  await Promise.all([b, c, d].map(enable));
  await Promise.all([b, c, d].map(async (peer, index) => {
    const plan = JSON.parse(scalar(await peer.query('EXPLAIN (ANALYZE, FORMAT JSON, TIMING OFF) ' + count)) as string)[0].Plan;
    const gather = nodes(plan).find(n => n['Node Type'] === 'Gather');
    assert(gather?.['Workers Planned'] === WORKERS && gather['Workers Launched'] === 0,
      'Two occupied parallel workers cause a safe serial fallback');
    assert(scalar(await peer.query(count)) === '20001', 'Fallback preserves query results');
    events().push({kind: 'parallel-budget-fallback', label, peer: index, plan});
  }));
  await c.close(); await d.close();
  await request.cancel();
  const canceled = await pending;
  assert(canceled instanceof DatabaseError && canceled.sqlstate === '57014', 'Parallel cancellation SQLSTATE');
  assert((await a.status()).transaction === 3, 'Cancellation aborts the owning transaction');
  await a.query('ROLLBACK'); await workerState(b, pid, 0);
  checks.push('parallel-cancel', 'parallel-peer-progress', 'parallel-worker-budget-fallback');

  await a.query("SET statement_timeout = '2s'");
  const timed = a.query(slow).then(value => value, error => error);
  await workerState(b, pid, WORKERS);
  const timeout = await timed;
  assert(timeout instanceof DatabaseError && timeout.sqlstate === '57014' && timeout.message.includes('statement timeout'), 'Parallel statement timeout');
  await a.query('SET statement_timeout = 0'); await workerState(b, pid, 0);
  checks.push('parallel-timeout');

  const failure = await errorState(() => a.query('SELECT sum(parallel_fail(id)) FROM parallel_fact'), '22012');
  assert(String(failure.diagnostics[8]).includes('parallel worker'), 'SQL error originated in a parallel worker');
  await workerState(b, pid, 0);
  await a.exec('SET parallel_leader_participation = on; SET debug_parallel_query = off');
  await explain(a, count, label + '-reuse-after-errors', aggregate);
  assert(scalar(await a.query(count)) === '20001', 'Parallel session remains reusable after cancel, timeout and worker error');
  checks.push('parallel-worker-error', 'parallel-reuse');
  await a.close(); await b.close();
}
export async function parallel(checks: string[], metrics: unknown[]) {
  for (const persistent of [false, true]) {
    const storage = persistent ? {type: 'opfs' as const, name: 'parallel-' + crypto.randomUUID()} : undefined;
    const db = await Database.open({storage});
    await queries(db, checks, persistent ? 'opfs' : 'ephemeral');
    metrics.push(await db.close());
    checks.push(persistent ? 'parallel-opfs' : 'parallel-ephemeral');
    if (storage) {
      const reopened = await Database.open({storage, create: false}), c = await reopened.connect();
      await enable(c);
      await explain(c, 'SELECT count(*) FROM parallel_fact', 'opfs-reopen', () => true);
      assert(scalar(await c.query('SELECT count(*) FROM parallel_fact')) === '20001', 'Parallel table survives persistent reopen');
      await c.close(); metrics.push(await reopened.close()); checks.push('parallel-opfs-reopen');
    }
  }
  for (const m of metrics as any[]) {
    assert(m.returnedWorkers === pthreadPoolSize && m.runningWorkers === 0, 'Parallel workers return to the fixed pool on close');
    assert(m.peakThreads <= pthreadPoolSize && m.peakMemory <= 536870912, 'Parallel execution stays within the resource budget');
    assert(Object.values(m.resources).every(n => n === 0), 'Parallel DSM and native resources are released');
  }
  checks.push('parallel-resource-release');
}
