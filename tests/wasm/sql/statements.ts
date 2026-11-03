/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
import {ClosedError} from '../src/api/database.js';
import type {Database} from '../src/api/database.js';
import {assert, errorState, scalar} from './helpers.js';

export async function statements(db: Database, checks: string[]) {
  const c = await db.connect('statement-owner'), peer = await db.connect('statement-peer');
  const statement = await c.prepare('SELECT $1::int8 AS exact, $2::text AS label');
  assert(String(statement.parameterTypes) === '20,25' && statement.columns[0].name === 'exact' &&
    statement.columns[1].typeOid === 25, 'Parse/Describe preserves inferred types and column metadata');
  for (const label of ['pi-π', null]) {
    const row = (await statement.query({parameters: [
      {typeOid: 20, value: '9223372036854775807'}, {typeOid: 25, value: label}]}))[0].rows[0];
    assert(row[0] === '9223372036854775807' && row[1] === label, 'One prepared plan executes with distinct bindings');
  }
  assert(Number(scalar(await c.query('SELECT count(*) FROM pg_prepared_statements'))) === 1,
    'A named server statement is reused');
  await statement.close(); await statement.close();
  assert(scalar(await c.query('SELECT count(*) FROM pg_prepared_statements')) === '0', 'Close releases the server plan');
  let closed = false;
  try {await statement.query();} catch (error) {closed = error instanceof ClosedError;}
  assert(closed, 'Closed statement handles cannot be reused');
  checks.push('prepared-inferred-metadata', 'prepared-reuse', 'prepared-close');

  const ordered = await c.prepare('SELECT $1::int4');
  const beforeClose = ordered.query({parameters: [{typeOid: 23, value: '42'}]});
  const orderedClose = ordered.close();
  assert(scalar(await beforeClose) === '42', 'Close preserves queries admitted before it');
  await orderedClose;
  checks.push('prepared-queued-close-order');

  const explicit = await c.prepare('SELECT $1 AS data', {parameterTypes: [17]});
  const bytes = new Uint8Array([0, 127, 128, 255]);
  const binary = (await explicit.query({binary: true, parameters: [{typeOid: 17, value: bytes}]}))[0].rows[0][0];
  assert(binary instanceof Uint8Array && String(binary) === String(bytes), 'Explicit parameter type and binary results');
  await explicit.close();
  const fallible = await c.prepare('SELECT 12 / $1::int4');
  await errorState(() => fallible.query({parameters: [{typeOid: 23, value: '0'}]}), '22012');
  assert(scalar(await fallible.query({parameters: [{typeOid: 23, value: '3'}]})) === '4', 'Execution error preserves the statement');
  await c.query('BEGIN');
  await errorState(() => fallible.query({parameters: [{typeOid: 23, value: '0'}]}), '22012');
  await fallible.close();
  await c.query('ROLLBACK');
  await errorState(() => c.prepare('SELECT missing_statement_column'), '42703');
  assert(scalar(await c.query('SELECT count(*) FROM pg_prepared_statements')) === '0', 'Failed Parse releases statement ownership');
  checks.push('prepared-explicit-binary', 'prepared-error-reuse', 'prepared-close-aborted-transaction', 'prepared-failed-prepare');

  const stream = await c.prepare('SELECT g FROM generate_series(1, $1::int4) g');
  let count = 0;
  for await (const batch of await stream.execute({chunkRows: 7, parameters: [{typeOid: 23, value: '31'}]}))
    for (const row of batch.rows) assert(row[0] === String(++count), 'Prepared stream preserves ordered values');
  assert(count === 31, 'Prepared stream completes');
  for await (const batch of await stream.execute({chunkRows: 8, parameters: [{typeOid: 23, value: '1000000'}]})) {
    assert(batch.rows.length === 8, 'Prepared stream yields before cancellation'); break;
  }
  assert(scalar(await stream.query({parameters: [{typeOid: 23, value: '1'}]})) === '1', 'Prepared plan survives early stream retirement');
  await stream.close();
  checks.push('prepared-stream', 'prepared-stream-cancel-reuse');

  await c.query('CREATE TABLE statement_locked(id int)');
  await peer.query('BEGIN'); await peer.query('LOCK statement_locked IN ACCESS EXCLUSIVE MODE');
  const pending = c.prepare('SELECT * FROM statement_locked');
  const pid = (await c.status()).pid;
  let waiting = false;
  const deadline = performance.now() + 10000;
  while (!waiting && performance.now() < deadline) {
    waiting = scalar(await peer.query(`SELECT wait_event_type = 'Lock' FROM pg_stat_activity WHERE pid = $1::int4`,
      {parameters: [{typeOid: 23, value: String(pid)}]})) === 't';
    if (!waiting) await new Promise(resolve => setTimeout(resolve, 1));
  }
  assert(waiting, 'Prepare is observably waiting while the other session remains responsive');
  await peer.query('ROLLBACK');
  const unlocked = await pending;
  assert((await unlocked.query())[0].rows.length === 0, 'Prepare resumes after peer releases the lock');
  await c.close(); await peer.close();
  assert((await db.telemetry()).requests === 0n, 'Connection close retires statements and requests');
  checks.push('prepared-lock-wait-yields-driver', 'prepared-connection-close');
}
