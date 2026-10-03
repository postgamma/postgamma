/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
import {Database, DatabaseError, version} from '@postgamma/wasm';
window.probeResult = null;
const metrics = [], timings = {}, checks = [];
const persistent = new URL(location.href).searchParams.get('storage') !== 'ephemeral';
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
    const storage = persistent ? {type: 'opfs', name} : undefined;
    const db = await Database.open({storage});
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
    const request = await a.execute('SELECT pg_sleep(10)');
    const waiting = request.next().catch(error => error);
    const pid = (await a.status()).pid, cancelDeadline = performance.now() + 5000;
    let sleeping = false;
    while (!sleeping && performance.now() < cancelDeadline)
      sleeping = scalar(await b.query(`SELECT EXISTS (SELECT FROM pg_stat_activity WHERE pid = ${pid} AND wait_event = 'PgSleep')::text`)) === 'true';
    assert(sleeping, 'Cancellation targets a running PostgreSQL sleep');
    const cancelStart = performance.now();
    await request.cancel();
    const canceled = await waiting;
    timings.cancelMs = performance.now() - cancelStart;
    assert(canceled instanceof DatabaseError && canceled.sqlstate === '57014', 'Cancellation');
    await request.close(); checks.push('sql-error-and-cancel-reuse');
    stage('scripts-notifications-capacity');
    const script = await a.exec('SELECT 21; SELECT 22');
    assert(script.length === 2 && script[0].rows[0][0] === '21' && script[1].rows[0][0] === '22', 'Installed script API');
    await a.query('LISTEN package_notification');
    let remove, deadlineTimer;
    const notification = new Promise((resolve, reject) => {
      deadlineTimer = setTimeout(() => reject(new Error('Installed notification timeout')), 5000);
      remove = a.onNotification(resolve, reject);
    });
    try {
      await b.exec("NOTIFY package_notification, 'installed'");
      const value = await notification;
      assert(value.channel === 'package_notification' && value.payload === 'installed', 'Installed idle notification API');
    } finally {clearTimeout(deadlineTimer); remove();}
    await a.query('UNLISTEN package_notification');
    const large = 'p'.repeat(5 * 1024 * 1024);
    assert(scalar(await a.query('SELECT $1::text', {parameters: [{typeOid: 25, value: large}]})) === large, 'Installed large-value contract');
    checks.push('script-api', 'notification-api', 'large-value-api');
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
    timings.selectOneMs = [];
    for (let i = 0; i < 20; i++) {
      const begin = performance.now();
      assert(scalar(await a.query('SELECT 1')) === '1', 'Small query');
      timings.selectOneMs.push(performance.now() - begin);
    }
    stage('idle');
    await db.telemetry();
    const idleStart = performance.now(), idleBefore = db.sample();
    await new Promise(resolve => setTimeout(resolve, 1500));
    await db.telemetry();
    const idleAfter = db.sample();
    timings.idle = {elapsedMs: performance.now() - idleStart,
      driverWakes: (idleAfter.driverWakeCount - idleBefore.driverWakeCount) >>> 0,
      periodicSamples: idleAfter.periodicSamples, visibility: document.visibilityState};
    assert(timings.idle.periodicSamples === 0 && timings.idle.driverWakes / (timings.idle.elapsedMs / 1000) < 100,
      'Default installed package waits for notifications while idle');
    checks.push('idle-notification-wait', 'running-query-cancel');
    stage('close');
    await Promise.all(connections.map(c => c.close())); metrics.push(await db.close());
    const reopen = performance.now();
    stage('reopen');
    const again = await Database.open({create: !persistent, storage,
      assetBase: new URL('./node_modules/@postgamma/wasm/', location.href)});
    timings.reopenMs = performance.now() - reopen;
    const c = await again.connect();
    if (persistent) {
    assert(scalar(await c.query('SELECT id FROM package_rows')) === '9223372036854775807', 'Persistent committed row');
    if (manifest.extensions.pgvector) {
      await c.query('SET enable_seqscan = off');
      assert(scalar(await c.query("SELECT id FROM postgamma_release_vector_hnsw ORDER BY embedding <-> '[1,1,1]' LIMIT 1")) === '1', 'Persistent vector index');
    }
    } else {
      assert(scalar(await c.query("SELECT to_regclass('package_rows')::text")) === null, 'Fresh ephemeral database');
    }
    await c.close();
    stage('archive');
    const clone = await again.clone(); metrics.push(await again.close());
    const copied = await clone.connect();
    assert(persistent ? scalar(await copied.query('SELECT count(*) FROM package_rows')) === '1' :
      scalar(await copied.query("SELECT to_regclass('package_rows')::text")) === null, 'Installed archive/restore/clone API');
    await copied.close(); metrics.push(await clone.close()); checks.push('archive-api');
    checks.push(persistent ? 'opfs-reopen-relocated-assets' : 'ephemeral-reopen-relocated-assets');
    assert(metrics.every(m => m.peakThreads <= 13 && m.peakMemory <= 536870912 &&
      m.returnedWorkers === 13 && m.runningWorkers === 0 && Object.values(m.resources).every(v => v === 0)), 'Healthy package resource release');
    window.probeResult = {status: 'passed', checks, version, buildId: manifest.buildId, metrics, timings, ticks};
  }
} catch (error) {window.probeResult = {status: 'failed', checks, error: String(error), stack: error.stack,
  sqlstate: error.sqlstate, statusCode: error.status, diagnostics: error.diagnostics, metrics, timings, ticks};}
finally {clearInterval(timer);}
