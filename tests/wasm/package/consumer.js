/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
import {Database, DatabaseError, version} from '@postgamma/wasm';
window.probeResult = null;
const metrics = [], timings = {}, checks = [];
window.probeProgress = {checks, timings, stage: 'manifest'};
const stage = name => {window.probeProgress.stage = name;};
let ticks = 0; const timer = setInterval(() => ticks++, 1);
const assert = (condition, message) => {if (!condition) throw new Error(message);};
const scalar = result => result[0].rows[0][0];
try {
  if (new URL(location.href).searchParams.has('fault')) {
    try {const db = await Database.open(); await db.close(); window.probeResult = {status: 'unexpected-success'};}
    catch (error) {window.probeResult = {status: 'rejected', error: String(error)};}
  } else {
    const manifest = await (await fetch('./node_modules/@postgamma/wasm/postgamma.assets.json')).json();
    assert(version === manifest.version && !manifest.bridge.testing, 'Production package version and bridge');
    const name = 'package-' + crypto.randomUUID(), start = performance.now();
    stage('open');
    const db = await Database.open({storage: {type: 'opfs', name}});
    timings.createMs = performance.now() - start;
    stage('connect');
    const connections = await Promise.all(Array.from({length: 4}, () => db.connect()));
    const [a,b] = connections;
    await a.query('CREATE TABLE package_rows (id bigint PRIMARY KEY)');
    await a.query('BEGIN'); await a.query('INSERT INTO package_rows VALUES (1)');
    assert(scalar(await b.query('SELECT count(*) FROM package_rows')) === '0', 'Independent transaction visibility');
    await a.query('ROLLBACK');
    await a.query('INSERT INTO package_rows VALUES ($1::int8)', {parameters: [{typeOid: 20, value: '9223372036854775807'}]});
    assert(scalar(await b.query('SELECT id FROM package_rows')) === '9223372036854775807', 'Exact int64 and committed visibility');
    checks.push('parameters-exact-values-transactions');
    stage('concurrency');
    const concurrent = connections.map(c => c.query('SELECT pg_sleep(0.3), 7'));
    const deadline = performance.now() + 5000;
    let live; do {live = await db.telemetry();} while (live.running < 4n && performance.now() < deadline);
    assert(live.running === 4n, 'Four actual concurrent sessions');
    await Promise.all(concurrent); checks.push('four-live-sessions');
    try {await a.query('SELECT 1/0'); throw new Error('Expected SQL error');}
    catch (e) {assert(e instanceof DatabaseError && e.sqlstate === '22012', 'SQLSTATE propagation');}
    assert(scalar(await a.query('SELECT 9')) === '9', 'SQL error reuse');
    stage('cancel');
    const request = await a.execute('SELECT pg_sleep(10)'); await request.cancel();
    try {await request.next(); throw new Error('Expected cancellation');}
    catch (e) {assert(e instanceof DatabaseError && e.sqlstate === '57014', 'Cancellation');}
    await request.close(); checks.push('sql-error-and-cancel-reuse');
    if (manifest.extensions.pgvector) {
      stage('vector');
      await a.query('CREATE EXTENSION vector');
      const fixture = await (await fetch('./vector-fixtures.json')).json();
      const begin = performance.now();
      for (const sql of fixture.nativeSetup) await a.query(sql);
      timings.vectorBuildMs = performance.now() - begin;
      await a.query('SET enable_seqscan = off');
      for (const suffix of ['hnsw','ivf']) {
        const table = 'postgamma_release_vector_' + suffix;
        const sql = `SELECT id FROM ${table} ORDER BY embedding <-> '[1,1,1]' LIMIT 1`;
        assert(scalar(await a.query(sql)) === '1', 'Indexed nearest vector');
        assert(scalar(await a.query('EXPLAIN (FORMAT JSON) ' + sql)).includes(table + '_idx'), 'Actual index plan');
      }
      checks.push('pgvector-hnsw-ivfflat');
    }
    timings.queryMs = [];
    for (let i = 0; i < 20; i++) {
      const begin = performance.now(); assert(scalar(await a.query('SELECT sum(i) FROM generate_series(1,10000) i')) === '50005000', 'Representative query');
      timings.queryMs.push(performance.now() - begin);
    }
    stage('close');
    await Promise.all(connections.map(c => c.close())); metrics.push(await db.close());
    const reopen = performance.now();
    stage('reopen');
    const again = await Database.open({create: false, storage: {type: 'opfs', name},
      assetBase: new URL('./node_modules/@postgamma/wasm/', location.href)});
    timings.reopenMs = performance.now() - reopen;
    const c = await again.connect();
    assert(scalar(await c.query('SELECT id FROM package_rows')) === '9223372036854775807', 'Persistent committed row');
    if (manifest.extensions.pgvector) {
      await c.query('SET enable_seqscan = off');
      assert(scalar(await c.query("SELECT id FROM postgamma_release_vector_hnsw ORDER BY embedding <-> '[1,1,1]' LIMIT 1")) === '1', 'Persistent vector index');
    }
    await c.close(); metrics.push(await again.close()); checks.push('opfs-reopen-relocated-assets');
    assert(metrics.every(m => m.peakThreads <= 13 && m.peakMemory <= 536870912 &&
      m.returnedWorkers === 13 && m.runningWorkers === 0 && Object.values(m.resources).every(v => v === 0)), 'Healthy package resource release');
    window.probeResult = {status: 'passed', checks, version, buildId: manifest.buildId, metrics, timings, ticks};
  }
} catch (error) {window.probeResult = {status: 'failed', checks, error: String(error.stack || error), metrics, timings, ticks};}
finally {clearInterval(timer);}
