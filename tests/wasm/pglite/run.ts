/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0
 * Modified from PGlite test scenarios; see NOTICE, upstream.json and cases.json.
 * Runs the real PostGamma API. No PGlite implementation or compatibility shim.
 */
import {Database, DatabaseError, ClosedError} from '../src/api/database.js';
import type {Connection, Parameter, Result} from '../src/api/database.js';
import {limits} from '../src/protocol/limits.js';

type Value = string | number | null;
interface Step {
  sql: string | string[]; rows?: Value[][]; columns?: string[]; oids?: number[]; commands?: string[];
  parameters?: {typeOid: number; value: string | number[] | null}[];
  sqlstate?: string; tolerance?: number;
}
interface Case {
  id: string; source: string; upstreamTest: string; steps?: Step[]; cleanup?: string[];
  kind?: string; mode?: string; count?: number; bytes?: number;
}
interface CaseResult {id: string; status: string; elapsedMs: number; error?: string; cleanupError?: string}
const page = window as unknown as {
  probeResult: unknown; pgliteEvent(event: unknown): Promise<void>;
  pgliteSnapshot(): Promise<unknown>;
};
page.probeResult = null;
const storageName = new URL(location.href).searchParams.get('storage') || 'ephemeral';
const storage = storageName === 'opfs' ? {type: 'opfs' as const, name: 'pglite-' + crypto.randomUUID()} : undefined;
const caseResults: CaseResult[] = [], metrics: unknown[] = [];
let database: Database | undefined, vectorOid = '', phase = 'load';
page.pgliteSnapshot = async () => {
  // Failure observation must not submit a command to the stalled driver.
  return JSON.parse(JSON.stringify({phase, sample: database?.sample(), metrics: database?.diagnostics()},
    (_, value) => typeof value === 'bigint' ? String(value) : value));
};
function assert(value: unknown, message: string): asserts value {if (!value) throw new Error(message);}
function errorText(error: unknown): string {
  // Firefox/WebKit stacks do not necessarily include the error message.
  const details = [String(error)];
  if (error instanceof DatabaseError) details.push(`SQLSTATE=${error.sqlstate}; status=${error.status}; diagnostics=${JSON.stringify(error.diagnostics)}`);
  if (error instanceof Error && error.stack) details.push(error.stack);
  return details.join('\n');
}
function equal(actual: unknown, expected: unknown, message: string) {
  assert(JSON.stringify(actual) === JSON.stringify(expected),
    `${message}: expected ${JSON.stringify(expected)}, got ${JSON.stringify(actual)}`);
}
function rows(results: Result[]) {return results.flatMap(r => r.rows);}
async function event(kind: string, details: object = {}) {
  await page.pgliteEvent({kind, phase, timeMs: performance.now(), ...details});
}
async function rejected(action: () => Promise<unknown>, accept: (error: unknown) => boolean, message: string) {
  try {await action();} catch (error) {assert(accept(error), `${message}: ${String(error)}`); return;}
  throw new Error(message + ': unexpectedly succeeded');
}
async function step(c: Connection, value: Step, schema: string) {
  const statements = (Array.isArray(value.sql) ? value.sql : [value.sql]).map(sql => sql.replaceAll('{{schema}}', schema));
  const parameters: Parameter[] | undefined = value.parameters?.map(p => ({...p,
    value: Array.isArray(p.value) ? new Uint8Array(p.value) : p.value}));
  if (value.sqlstate) {
    assert(statements.length === 1, 'SQL error fixture has one statement');
    await rejected(() => c.query(statements[0], {parameters}),
      e => e instanceof DatabaseError && e.sqlstate === value.sqlstate, 'SQLSTATE ' + value.sqlstate);
    return;
  }
  const results: Result[] = [];
  for (const sql of statements) results.push(...await c.query(sql, {parameters}));
  if (value.rows) {
    const actual = rows(results);
    equal(actual.length, value.rows.length, 'row count');
    for (let i = 0; i < value.rows.length; i++) {
      equal(actual[i].length, value.rows[i].length, 'column count');
      for (let j = 0; j < value.rows[i].length; j++) {
        const expected = value.rows[i][j], observed = actual[i][j];
        if (typeof expected === 'number') {
          assert(typeof observed === 'string' && Number.isFinite(Number(observed)) &&
            Math.abs(Number(observed) - expected) <= (value.tolerance ?? 0), `numeric value ${observed} != ${expected}`);
        } else equal(observed, expected, `row ${i} column ${j}`);
      }
    }
  }
  const columns = results.find(r => r.columns.length)?.columns;
  if (value.columns) equal(columns?.map(c => c.name), value.columns, 'column names');
  if (value.oids) equal(columns?.map(c => c.typeOid), value.oids, 'column type OIDs');
  if (value.commands) equal(results.filter(r => r.command).map(r => r.command), value.commands, 'command tags');
}
async function copy(c: Connection) {
  await c.query('CREATE TABLE test(id SERIAL PRIMARY KEY,test TEXT)');
  await c.query('CREATE TABLE test2(id INT,test TEXT)');
  await c.query("INSERT INTO test(test) VALUES('test'),('test2')");
  const output = await c.execute('COPY (SELECT * FROM test ORDER BY id) TO STDOUT WITH (FORMAT csv)');
  let csv = '';
  try {
    const stream = (await output.next())?.copy;
    assert(stream, 'COPY OUT stream');
    for (;;) {const part = await stream.read(7); if (part === null) break; csv += new TextDecoder().decode(part);}
    await stream.close();
    const terminal: string[] = []; for (;;) {const r = await output.next(); if (!r) break; if (r.command) terminal.push(r.command);}
    equal(terminal, ['COPY 2'], 'COPY OUT row count');
  } finally {await output.close();}
  equal(csv, '1,test\n2,test2\n', 'upstream COPY bytes');
  const input = await c.execute('COPY test2 FROM STDIN WITH (FORMAT csv)');
  try {
    const stream = (await input.next())?.copy;
    assert(stream, 'COPY IN stream');
    await stream.write(new TextEncoder().encode(csv)); await stream.finish(); await stream.close();
    const terminal: string[] = []; for (;;) {const r = await input.next(); if (!r) break; if (r.command) terminal.push(r.command);}
    equal(terminal, ['COPY 2'], 'COPY IN row count');
  } finally {await input.close();}
  equal(rows(await c.query('SELECT * FROM test2 ORDER BY id')), [['1', 'test'], ['2', 'test2']], 'COPY roundtrip');
}
async function special(c: Connection, test: Case) {
  if (test.kind === 'copy') return copy(c);
  if (test.kind === 'closed') {
    const other = await database!.connect(); await other.close();
    await rejected(() => other.query('SELECT 1'), e => e instanceof ClosedError, 'closed connection rejects query');
  } else if (test.kind === 'memory') {
    await c.query('CREATE TABLE leak_test(id SERIAL PRIMARY KEY,blob JSONB NOT NULL)');
    const blob = JSON.stringify({padding: 'x'.repeat(100 * 1024)});
    for (let i = 0; i < 300; i++) await c.query(`INSERT INTO leak_test(blob) VALUES('${blob}')`);
    const result = rows(await c.query("SELECT used_bytes FROM pg_backend_memory_contexts WHERE name='MessageContext' ORDER BY level LIMIT 1"));
    assert(result.length === 1 && Number(result[0][0]) < 5 * 1024 * 1024, 'MessageContext stays below upstream 5 MiB bound');
    equal(rows(await c.query('SELECT count(*) FROM leak_test')), [['300']], 'all memory-test inserts committed');
  } else if (test.kind === 'size') {
    const data = 'a'.repeat(test.bytes!);
    await c.query('CREATE TABLE size_test(id SERIAL PRIMARY KEY,data TEXT)');
    let result: Result[];
    if (test.mode === 'exec') {
      await c.query(`INSERT INTO size_test(data) VALUES('${data}')`);
      result = await c.query('SELECT data FROM size_test');
    }
    else if (test.mode === 'generated') result = await c.query(`SELECT repeat('a',${test.bytes})`);
    else {
      await c.query(test.mode === 'literal' ? `INSERT INTO size_test(data) VALUES('${data}')` :
        'INSERT INTO size_test(data) VALUES($1)', test.mode === 'parameter' ? {parameters: [{typeOid: 25, value: data}]} : {});
      result = await c.query('SELECT data FROM size_test');
    }
    equal(rows(result), [[data]], 'payload roundtrip');
  } else if (test.kind === 'rows') {
    const data = 'a'.repeat(test.bytes!), count = test.count!;
    const sql = `SELECT g AS id, '${data}' AS data FROM generate_series(1,${count}) g`;
    const chunkRows = Math.max(1, Math.min(128, Math.floor(64 * 1024 / (test.bytes! + 256))));
    // Exercise the iterator as well. Bound transferred payload to 16 MiB per
    // case; larger upstream matrices above test rejection, not GiB capacity.
    const streamed = Math.min(count, Math.floor(16 * 1024 * 1024 / test.bytes!));
    await event('step', {id: test.id, operation: 'stream', streamedRows: streamed, chunkRows});
    const request = await c.execute(`SELECT g, '${data}' FROM generate_series(1,${streamed}) g`, {chunkRows});
    let seen = 0;
    try {for (;;) {
      const batch = await request.next(); if (batch === null) break;
      for (const row of batch.rows) {seen++; equal(row, [String(seen), data], 'streamed row');}
    }} finally {await request.close();}
    equal(seen, streamed, 'complete bounded stream');
    await event('step', {id: test.id, operation: 'collect', chunkRows});
    if (count * test.bytes! > limits.collectedResultBytes) {
      // Upstream collects up to 1 GiB. Assert PostGamma's bounded rejection.
      await rejected(() => c.query(sql, {chunkRows}), e => e instanceof Error && e.message.includes('Collected results exceed'), 'collected result limit');
      equal(rows(await c.query('SELECT 7')), [['7']], 'session reusable after result limit');
    } else {
      const result = rows(await c.query(sql, {chunkRows})); equal(result.length, count, 'collected row count');
      for (let i = 0; i < count; i++) equal(result[i], [String(i + 1), data], 'collected row');
    }
    await event('measurement', {id: test.id, collectedRows: count, streamedRows: streamed, bytesPerRow: test.bytes});
  } else if (test.kind === 'materialized') {
    await event('step', {id: test.id, operation: 'default-query'});
    const result = rows(await c.query(`SELECT g,repeat('a',${test.bytes}) FROM generate_series(1,${test.count}) g`));
    equal(result.length, test.count, 'default materialized row count');
    for (let i = 0; i < result.length; i++) equal(result[i], [String(i + 1), 'a'.repeat(test.bytes!)], 'default materialized row');
  } else if (test.kind === 'stream-after-limit') {
    const sql = "SELECT g,repeat('a',10240) FROM generate_series(1,1000) g";
    await event('step', {id: test.id, operation: 'collect-over-limit'});
    await rejected(() => c.query(sql, {chunkRows: 6}), e => e instanceof Error && e.message.includes('Collected results exceed'), 'collection limit');
    equal(rows(await c.query('SELECT 7')), [['7']], 'small query after cancellation');
    await event('step', {id: test.id, operation: 'stream-after-cancellation'});
    const request = await c.execute(sql, {chunkRows: 6}); let seen = 0;
    try {for await (const batch of request) for (const row of batch.rows) {
      seen++; equal(row, [String(seen), 'a'.repeat(10240)], 'stream after collection limit');
    }} finally {await request.close();}
    equal(seen, 1000, 'all rows after prior cancellation');
  } else if (test.kind === 'value-limit') {
    await rejected(() => c.query(`SELECT repeat('a',${limits.maximumValueBytes + 1})`),
      e => e instanceof DatabaseError && e.sqlstate === '54000', 'maximum value SQLSTATE');
    equal(rows(await c.query('SELECT 9')), [['9']], 'session reusable after value limit');
  } else throw new Error('Unknown case kind: ' + test.kind);
}
async function lifecycle(test: Case) {
  if (test.kind === 'database-close') {
    const c = await database!.connect();
    await c.query('CREATE TABLE public.pglite_reopen(id INT PRIMARY KEY,value TEXT)');
    await c.query("INSERT INTO public.pglite_reopen VALUES(1,'test')");
    vectorOid = String(rows(await c.query("SELECT oid FROM pg_extension WHERE extname='vector'"))[0][0]);
    await c.close();
    const closed = database!; metrics.push(await closed.close()); database = undefined;
    await rejected(() => closed.connect(), e => e instanceof ClosedError, 'closed database rejects connect');
    await rejected(() => c.query('SELECT 1'), e => e instanceof ClosedError, 'closed database connection rejects query');
  } else if (test.kind === 'reopen') {
    database = await Database.open({storage, ...(storage ? {create: false} : {})});
    const c = await database.connect();
    try {
      if (storage) equal(rows(await c.query('SELECT * FROM public.pglite_reopen')), [['1','test']], 'OPFS persisted row');
      else {
        equal(rows(await c.query("SELECT to_regclass('public.pglite_reopen')")), [[null]], 'new ephemeral database is empty');
        await c.query('CREATE EXTENSION vector');
      }
    } finally {await c.close();}
  } else if (test.kind === 'vector-reopen') {
    const c = await database!.connect();
    try {
      const oid = String(rows(await c.query("SELECT oid FROM pg_extension WHERE extname='vector'"))[0][0]);
      assert(Number(oid) >= 16384, 'extension OID exists after reopen');
      if (storage) equal(oid, vectorOid, 'extension OID unchanged after OPFS reopen');
      equal(rows(await c.query("SELECT '[1,2,3]'::vector <-> '[1,2,3]'::vector")), [['0']], 'vector usable after reopen');
    } finally {await c.close();}
  }
}
let failure: unknown;
try {
  assert(['ephemeral', 'opfs'].includes(storageName), 'known storage profile');
  const response = await fetch('./pglite-tests/cases.json'); assert(response.ok, 'case manifest fetch');
  const fixture = await response.json() as {schemaVersion: number; cases: Case[]};
  const startIndex = Number(new URL(location.href).searchParams.get('start') || 0);
  assert(Number.isSafeInteger(startIndex) && startIndex >= 0 && startIndex < fixture.cases.length, 'valid case start index');
  const cases = fixture.cases.slice(startIndex);
  assert(fixture.schemaVersion === 1 && cases.length > 0, 'nonempty known fixture version');
  phase = 'open'; await event('phase'); database = await Database.open({storage});
  const setup = await database.connect(); await setup.query('CREATE EXTENSION vector'); await setup.close();
  await event('ready', {ids: cases.map(c => c.id), storage: storageName});
  document.documentElement.dataset.pgliteReady = 'true';
  for (const [index, test] of cases.entries()) {
    phase = test.id; await event('case-start', {id: test.id});
    const start = performance.now(), result: CaseResult = {id: test.id, status: 'failed', elapsedMs: 0};
    let connection: Connection | undefined, schema = 'pglite_case_' + index, cleanupFailed = false;
    try {
      if (['database-close','reopen','vector-reopen'].includes(test.kind || '')) await lifecycle(test);
      else {
        connection = await database!.connect('pglite/' + test.id);
        for (const sql of [`CREATE SCHEMA ${schema}`, `SET search_path TO ${schema},public`, "SET TIME ZONE 'UTC'", "SET statement_timeout='15s'"])
          await connection.query(sql);
        if (test.steps) for (const [i, value] of test.steps.entries()) {
          await event('step', {id: test.id, step: i}); await step(connection, value, schema);
        } else await special(connection, test);
      }
      result.status = 'passed';
    } catch (error) {result.error = errorText(error);}
    finally {
      if (connection) {
        try {
          for (const sql of ['ROLLBACK', 'RESET ROLE', 'SET statement_timeout=0']) await connection.query(sql);
          for (const sql of test.cleanup || []) await connection.query(sql);
          await connection.query(`DROP SCHEMA ${schema} CASCADE`);
        } catch (error) {cleanupFailed = true; result.cleanupError = errorText(error); result.status = 'failed';}
        try {await connection.close();} catch (error) {cleanupFailed = true; result.cleanupError = errorText(error); result.status = 'failed';}
      }
      result.elapsedMs = performance.now() - start; caseResults.push(result); await event('case-end', {result});
      document.documentElement.dataset.pgliteCompleted = String(caseResults.length);
    }
    if (cleanupFailed || !database && test.kind !== 'database-close') throw new Error('Cannot safely continue after ' + test.id);
  }
  assert(caseResults.every(r => r.status === 'passed'), 'One or more imported cases failed');
} catch (error) {failure = error;}
finally {
  phase = 'close'; await event('phase');
  try {if (database) metrics.push(await database.close());}
  catch (error) {failure ??= error; await event('close-error', {error: errorText(error)});}
  const status = failure ? 'failed' : 'passed';
  page.probeResult = {status, storage: storageName, cases: caseResults, metrics,
    isolated: crossOriginIsolated, worker: typeof Worker === 'function', sharedMemory: typeof SharedArrayBuffer === 'function',
    error: failure ? errorText(failure) : undefined};
  await event('done', {result: page.probeResult});
}
