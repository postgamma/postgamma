/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
import {Database, DatabaseError} from '../src/api/database.js';
import type {Connection, Result} from '../src/api/database.js';
import {assert, scalar, errorState} from './helpers.js';
import {concurrency, saturated} from './concurrency.js';
import {errors} from './errors.js';
import {results} from './results.js';
import {lifecycle, fatal, closeAuditFailure, failedOpenRecovery} from './lifecycle.js';
import {budget} from './budget.js';
import {bindings} from './bindings.js';
import {storage} from '../storage-tests/run.js';
import {durability} from '../storage-tests/durability.js';
import {storageErrors} from '../storage-tests/errors.js';
import {storageBudget} from '../storage-tests/budget.js';
import {loadVector} from '../vector-tests/load.js';
import {vectorQueries} from '../vector-tests/query.js';
import {vectorStorage} from '../vector-tests/storage.js';
import {vectorExample} from '../vector-tests/example.js';
import {vectorBudget} from '../vector-tests/budget.js';
const page = window as unknown as {probeResult: unknown; probeEvents: unknown[]};
page.probeResult = null; page.probeEvents = [];
const scenario = new URL(location.href).searchParams.get('scenario') || 'abi';
let pageTicks = 0;
const timer = setInterval(() => pageTicks++, 1);
const checks: string[] = [];
async function abi(db: Database) {
  const c = await db.connect();
  const values = await c.query('SELECT $1::int8 AS exact, $2::numeric AS numeric, $3::text AS missing, $4::text AS unicode', {
    parameters: [{typeOid: 20, value: '9223372036854775807'},
      {typeOid: 1700, value: '12345678901234567890.123456789'}, {typeOid: 25, value: null},
      {typeOid: 25, value: 'caf\u00e9 \ud83d\udc18'}],
  });
  assert(JSON.stringify(values[0].rows[0]) === JSON.stringify([
    '9223372036854775807', '12345678901234567890.123456789', null, 'caf\u00e9 \ud83d\udc18']), 'Exact/null/Unicode values');
  assert(values[0].columns[0].typeOid === 20 && values[0].columns[0].name === 'exact', 'Column metadata');
  const bytes = new Uint8Array([0, 1, 127, 128, 255]);
  const binary = await c.query('SELECT $1::bytea', {binary: true, parameters: [{typeOid: 17, value: bytes}]});
  assert(String(binary[0].rows[0][0]) === String(bytes), 'Binary roundtrip');
  const empty = await c.query('SELECT $1::text, $2::bytea, $3::bytea', {binary: true, parameters: [
    {typeOid: 25, value: ''}, {typeOid: 17, value: new Uint8Array()}, {typeOid: 17, value: null}]});
  assert((empty[0].rows[0][0] as Uint8Array).length === 0 &&
    (empty[0].rows[0][1] as Uint8Array).length === 0 && empty[0].rows[0][2] === null, 'Empty differs from NULL');
  const e = await errorState(() => c.query('SELECT 1/0'), '22012');
  assert(e.diagnostics[1] === 'ERROR' && e.diagnostics[2] === 'division by zero', 'SQL diagnostics');
  const detail = await errorState(() => c.query("DO $$ BEGIN RAISE EXCEPTION 'message' USING DETAIL = 'detail', HINT = 'hint'; END $$"), 'P0001');
  assert(detail.diagnostics[3] === 'detail' && detail.diagnostics[4] === 'hint' && detail.diagnostics[8], 'Diagnostic detail/hint/context');
  assert(scalar(await c.query('SELECT 7')) === '7', 'Connection recovers after SQL ERROR');
  const old = await c.execute('SELECT 8'); await old.next(); await old.next(); await old.close();
  const fresh = await c.execute('SELECT 9');
  let staleRejected = false;
  try {await old.cancel();} catch (error) {staleRejected = error instanceof DatabaseError;}
  assert(staleRejected && scalar([await fresh.next() as Result]) === '9', 'Stale generation rejected without touching fresh request');
  await fresh.next(); await fresh.close();
  await c.close();
  checks.push('parameters-exact-null-unicode', 'binary-copy', 'column-metadata', 'sqlstate-diagnostics', 'stale-generation');
}
try {
  const metrics: unknown[] = [];
  if (scenario === 'storage') await storage(checks, metrics);
  else if (scenario === 'durability') await durability(checks, metrics);
  else if (scenario === 'storage-errors') await storageErrors(checks, metrics);
  else if (scenario === 'vector-storage') await vectorStorage(checks, metrics);
  else if (scenario === 'vector-example') await vectorExample(checks, metrics);
  else if (scenario === 'close-audit') metrics.push(await closeAuditFailure(checks));
  else if (scenario === 'open-failed') metrics.push(await failedOpenRecovery(checks));
  else {
  const storage = scenario.startsWith('fatal-opfs-') ? {type: 'opfs' as const, name: 'fatal-' + crypto.randomUUID()} : undefined;
  const db = await Database.open({storage});
  if (scenario === 'abi') await abi(db);
  else if (scenario === 'bindings') await bindings(db, checks);
  else if (scenario === 'vector-load') await loadVector(db, checks);
  else if (scenario === 'vector-query') await vectorQueries(db, checks);
  else if (scenario === 'concurrency') await concurrency(db, checks);
  else if (scenario === 'errors') await errors(db, checks);
  else if (scenario === 'results') await results(db, checks);
  else if (scenario === 'budget') await budget(db, checks);
  else if (scenario === 'lifecycle') await lifecycle(db, checks);
  else if (['fatal-trap', 'fatal-abort', 'fatal-opfs-trap', 'fatal-opfs-abort'].includes(scenario))
    await fatal(db, checks, scenario.endsWith('trap') ? 1 : 2, storage);
  else throw new Error(`Unknown SQL scenario: ${scenario}`);
  metrics.push(scenario.startsWith('fatal-') ? db.diagnostics() : await db.close());
  if (scenario === 'concurrency') {
    const small = await Database.open({workers: 2});
    await saturated(small, checks); metrics.push(await small.close());
  }
  }
  const resourceBudget = ['storage', 'durability', 'storage-errors'].includes(scenario) ? storageBudget(metrics, page.probeEvents) :
    scenario.startsWith('vector') || scenario === 'abi' && new URL(location.href).searchParams.get('extension') === 'pgvector'
      ? vectorBudget(metrics, scenario === 'vector-query') : undefined;
  page.probeResult = {status: 'passed', scenario, checks, metrics, pageTicks, resourceBudget,
    isolated: crossOriginIsolated, worker: true, sharedMemory: true};
} catch (error) {
  page.probeResult = {status: 'failed', scenario, checks, pageTicks,
    error: error instanceof Error ? error.stack : String(error), isolated: crossOriginIsolated};
} finally {clearInterval(timer);}
