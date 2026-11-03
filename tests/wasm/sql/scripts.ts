/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
import type {Connection, Database, Result} from '../src/api/database.js';
import {assert, scalar, errorState} from './helpers.js';
export async function scriptSemantics(c: Connection) {
  await c.query('CREATE TEMP TABLE script_values(id int PRIMARY KEY, value text)');
  const result = await c.exec("INSERT INTO script_values VALUES(1,'a;b'); SELECT value FROM script_values; SELECT 2 AS second");
  assert(JSON.stringify(result.map(r => r.command)) === JSON.stringify(['INSERT 0 1', 'SELECT 1', 'SELECT 1']), 'Script command order');
  assert(result[1].rows[0][0] === 'a;b' && result[2].rows[0][0] === '2', 'Script result boundaries');
  await c.exec("DO $$ BEGIN INSERT INTO script_values VALUES(2, 'dollar;quoted'); END $$; -- ;\nSELECT 3");
  assert(scalar(await c.query('SELECT count(*) FROM script_values')) === '2', 'Server parses quoted semicolons');
  assert((await c.exec(' ; -- empty\n ;')).every(r => !r.rows.length), 'Empty statements');
  await errorState(() => c.exec('INSERT INTO script_values VALUES(3,\'rollback\'); SELECT 1/0; INSERT INTO script_values VALUES(4,\'skipped\')'), '22012');
  assert(scalar(await c.query('SELECT count(*) FROM script_values')) === '2', 'Implicit script transaction rolls back and skips later statements');
  await errorState(() => c.exec("BEGIN; INSERT INTO script_values VALUES(5,'committed'); COMMIT; INSERT INTO script_values VALUES(6,'rollback'); SELECT 1/0"), '22012');
  assert(scalar(await c.query('SELECT count(*) FROM script_values WHERE id >= 5')) === '1', 'Explicit commit persists before later error');
  await errorState(() => c.exec("BEGIN; INSERT INTO script_values VALUES(7,'rollback'); SELECT 1/0; COMMIT"), '22012');
  await errorState(() => c.query('SELECT 1'), '25P02');
  await c.exec('ROLLBACK; SELECT 1');
  assert(scalar(await c.query('SELECT count(*) FROM script_values WHERE id = 7')) === '0', 'Aborted explicit transaction recovers with rollback');
  const streamed: Result[] = [];
  for await (const batch of await c.executeScript('SELECT g FROM generate_series(1,7) g; SELECT 8', {chunkRows: 3})) streamed.push(batch);
  assert(streamed.flatMap(r => r.rows).map(r => r[0]).join(',') === '1,2,3,4,5,6,7,8', 'Chunked script preserves every statement');
  assert(streamed.filter(r => r.command).map(r => r.command).join(',') === 'SELECT 7,SELECT 1', 'Chunked script terminal tags');
  const copy = await c.executeScript('COPY (SELECT 11) TO STDOUT; SELECT 12');
  try {
    const start = await copy.next(); assert(start?.copy, 'Script exposes COPY stream');
    let text = ''; for await (const bytes of start.copy) text += new TextDecoder().decode(bytes);
    const rest: Result[] = []; for await (const batch of copy) rest.push(batch);
    assert(text === '11\n' && rest.some(r => r.command === 'COPY 1') && scalar(rest) === '12', 'Script resumes after COPY');
  } finally {await copy.close();}
  await errorState(() => c.query('SELECT 1; SELECT 2'), '42601');
  let rejected = false;
  try {await c.exec('SELECT $1', {parameters: []} as never);} catch (error) {rejected = error instanceof TypeError;}
  assert(rejected, 'Script options cannot silently ignore parameters');
  assert(scalar(await c.query('SELECT 13')) === '13', 'Session reusable after script errors');
}
export async function scripts(db: Database, checks: string[]) {
  const c = await db.connect(), observer = await db.connect();
  try {
    await scriptSemantics(c);
    const pid = (await c.status()).pid;
    const request = await c.executeScript("INSERT INTO script_values VALUES(9,'cancel'); SELECT pg_sleep(30); INSERT INTO script_values VALUES(10,'skipped')");
    try {
      const deadline = performance.now() + 5000; let sleeping = false;
      do {sleeping = scalar(await observer.query(`SELECT wait_event = 'PgSleep' FROM pg_stat_activity WHERE pid = ${pid}`)) === 't';}
      while (!sleeping && performance.now() < deadline);
      assert(sleeping, 'Cancellation reaches an executing script');
      await request.cancel();
      await errorState(async () => {while (await request.next() !== null) {}}, '57014');
    } finally {await request.close();}
    assert(scalar(await c.query('SELECT count(*) FROM script_values WHERE id >= 9')) === '0', 'Cancelled script rolls back and suppresses later statements');
    checks.push('script-statement-boundaries', 'script-implicit-rollback', 'script-explicit-transaction',
      'script-chunked-results', 'script-copy', 'script-cancel-rollback', 'script-parameter-boundary');
  } finally {await c.close(); await observer.close();}
}
