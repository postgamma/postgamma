/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
import {DatabaseError} from '../src/api/database.js';
import type {Database, Result} from '../src/api/database.js';
import {assert, scalar, errorState} from './helpers.js';
import {collect} from './errors.js';
import {limits} from '../src/protocol/limits.js';
export async function results(db: Database, checks: string[]) {
  const fixture = await (await fetch(new URL('../fixtures.json', import.meta.url))).json();
  const oracle = await (await fetch(new URL('../oracle.json', import.meta.url))).json();
  const c = await db.connect('stream-owner'), observer = await db.connect('stream-observer');
  // Both results fit in the 64 KiB transport. Let the backend finish before
  // consuming: libpq can read only part of an already-readable transport and
  // return AGAIN after the producer has sent its last notification.
  const pid = (await c.status()).pid;
  for (const chunkRows of [0, 2]) {
    const count = chunkRows ? 3 : 400, width = chunkRows ? 10000 : 100;
    const sql = `SELECT g, repeat('a', ${width}) FROM generate_series(1, ${count}) g`;
    const request = await c.execute(sql, chunkRows ? {chunkRows} : {});
    const deadline = performance.now() + 5000;
    let idle = false;
    do {
      idle = scalar(await observer.query(`SELECT state = 'idle' AND query = $2::text
        FROM pg_stat_activity WHERE pid = $1::int`, {
        parameters: [{typeOid: 23, value: String(pid)}, {typeOid: 25, value: sql}],
      })) === 't';
      if (!idle) await new Promise(resolve => setTimeout(resolve, 1));
    } while (!idle && performance.now() < deadline);
    assert(idle, 'Producer completes before the first result read');
    let received = 0;
    for await (const batch of request) {
      for (const row of batch.rows) {
        assert(row[0] === String(++received) && row[1] === 'a'.repeat(width),
          'Already-buffered results arrive once and in order');
      }
    }
    assert(received === count, 'Drains readable input without another producer notification');
  }
  checks.push('materialized-level-readiness', 'chunked-level-readiness');
  for (const [name, chunkRows, binary] of [['basic', 3, false], ['binary', 2, true]] as const) {
    const request = await c.execute(fixture[name], {chunkRows, binary});
    const values: string[] = [], chunks: number[] = [];
    for await (const batch of request) {
      chunks.push(batch.rows.length);
      for (const row of batch.rows) {
        const cell = row[0];
        values.push(binary ? String(new DataView((cell as Uint8Array).buffer,
          (cell as Uint8Array).byteOffset, 4).getInt32(0, false)) : String(cell));
      }
    }
    assert(JSON.stringify(values) === JSON.stringify(oracle[name]), `${name} matches native PostgreSQL`);
    assert(JSON.stringify(chunks) === JSON.stringify(binary ? [2, 2, 1, 0] : [3, 3, 3, 1, 0]), 'Native chunk and terminal sequence');
  }
  let lateRows = 0;
  await errorState(async () => {
    for await (const batch of await c.execute(fixture.lateError, {chunkRows: 3})) lateRows += batch.rows.length;
  }, '22012');
  assert(lateRows === 6 && scalar(await c.query('SELECT 1')) === '1', 'Late error preserves preceding complete batches and connection reuse');
  checks.push('native-text-oracle', 'native-binary-oracle', 'chunk-terminal-order', 'late-stream-error');

  const stalled = await c.execute("SELECT g, repeat('q', 1024) FROM generate_series(1, 100000) g", {chunkRows: 32});
  const retained = await stalled.next();
  assert(retained?.rows.length === 32, 'First bounded result batch');
  const baseline = db.sample()!.memory;
  await new Promise(resolve => setTimeout(resolve, 150));
  await observer.query('SELECT 2');
  const afterStall = db.sample()!;
  assert(afterStall.memory - baseline <= 16 * 1024 * 1024 && afterStall.queuedBytes <= 8 * 1024 * 1024, 'Stalled consumer has bounded queues and memory');
  assert(retained.rows[0][1] === 'q'.repeat(1024), 'Retained batch owns stable bytes');
  await stalled.close();
  assert(scalar(await c.query('SELECT 3')) === '3', 'Early result close retires the producer');
  let total = 0;
  for await (const batch of await c.execute('SELECT g FROM generate_series(1, 20000) g', {chunkRows: 128})) {
    total += batch.rows.length;
    if (total % 1024 === 0) await new Promise(resolve => setTimeout(resolve, 1));
  }
  assert(total === 20000, 'Bounded iterator drains all rows');
  let collectionBound = false;
  try {await c.query(`SELECT repeat('q', 1024) FROM generate_series(1, ${Math.ceil(limits.collectedResultBytes / 1024) + 1})`, {chunkRows: 32});}
  catch (error) {collectionBound = error instanceof Error && error.message.includes('Collected results exceed');}
  assert(collectionBound && scalar(await c.query('SELECT 4')) === '4', 'Collected-result limit cancels safely');
  const canceled = await c.execute('SELECT generate_series(1, 10000000)', {chunkRows: 32});
  await canceled.next(); await canceled.cancel();
  await errorState(() => collect(canceled), '57014');
  checks.push('stalled-consumer-bound', 'early-result-close', 'full-bounded-iterator', 'collection-limit');
  checks.push('cancel-result-stream');

  await c.query(fixture.copyCreate);
  const input = new TextEncoder().encode(oracle.copy);
  const copyIn = await c.execute(fixture.copyIn), startIn = await copyIn.next();
  assert(startIn?.copy?.direction === 'in', 'COPY IN owns a stream handle');
  await startIn.copy.write(input); await startIn.copy.finish(); await startIn.copy.close();
  const inputTerminal = await collect(copyIn);
  assert(inputTerminal[0].command === 'COPY 3', 'COPY IN terminal command');
  const copyOut = await c.execute(fixture.copyOut), startOut = await copyOut.next();
  assert(startOut?.copy?.direction === 'out', 'COPY OUT owns a stream handle');
  const parts: Uint8Array[] = [];
  for (;;) {const part = await startOut.copy.read(257); if (!part) break; parts.push(part);}
  await startOut.copy.close();
  assert((await collect(copyOut))[0].command === 'COPY 3', 'COPY OUT terminal command');
  const output = new Uint8Array(parts.reduce((sum, p) => sum + p.length, 0));
  let offset = 0; for (const part of parts) {output.set(part, offset); offset += part.length;}
  assert(output.length === input.length && output.every((value, index) => value === input[index]), 'COPY bytes match native PostgreSQL oracle');
  checks.push('native-copy-oracle', 'copy-partial-io', 'copy-terminal-order');

  const early = await c.execute("COPY (SELECT g, repeat('q', 4096) FROM generate_series(1, 100000) g) TO STDOUT");
  const earlyCopy = (await early.next())!.copy!;
  const kept = await earlyCopy.read(); assert(kept && kept.length > 0, 'COPY produced a bounded chunk');
  const keptSnapshot = kept.slice();
  await new Promise(resolve => setTimeout(resolve, 100));
  await early.close();
  assert(scalar(await c.query('SELECT 5')) === '5', 'Early COPY close cancels a backpressured producer');
  await c.query('CREATE TABLE copy_reject(id integer CHECK (id < 2))');
  const rejected = await c.execute('COPY copy_reject FROM STDIN');
  const rejectCopy = (await rejected.next())!.copy!;
  await rejectCopy.write(new TextEncoder().encode('1\n2\n')); await rejectCopy.finish(); await rejectCopy.close();
  await errorState(() => collect(rejected), '23514');
  assert(scalar(await c.query('SELECT count(*) FROM copy_reject')) === '0', 'COPY error rolls back all input rows');
  const aborted = await c.execute('COPY copy_reject FROM STDIN');
  const abortCopy = (await aborted.next())!.copy!;
  await abortCopy.write(new TextEncoder().encode('1\n')); await abortCopy.abort(); await abortCopy.close();
  await errorState(() => collect(aborted), '57014');
  assert(scalar(await c.query('SELECT count(*) FROM copy_reject')) === '0', 'Aborted COPY does not publish partial input');
  checks.push('early-copy-close', 'copy-error-rollback');
  checks.push('abort-copy-input');

  const owned = (await c.query("SELECT decode('0080ff', 'hex')", {binary: true}))[0].rows[0][0] as Uint8Array;
  const beforeGrowth = db.sample()!.memory;
  await errorState(() => c.query("SELECT array_agg(repeat('x', 1024)) FROM generate_series(1, 32768)"), '54000');
  const afterGrowth = db.sample()!.memory;
  assert(afterGrowth > beforeGrowth, 'SQL allocations actually grow linear memory');
  assert(String(owned) === '0,128,255' && kept!.every((value, index) => value === keptSnapshot[index]), 'Owned buffers survive actual Wasm memory growth and native result release');
  assert(scalar(await c.query('SELECT 6')) === '6', 'Value-limit ERROR is recoverable');
  (window as unknown as {probeEvents: unknown[]}).probeEvents.push({kind: 'stream-memory',
    baseline, afterStall: afterStall.memory, beforeGrowth, afterGrowth, rows: total, copyBytes: output.length});
  checks.push('maximum-value-limit', 'owned-bytes-after-memory-growth');
  await c.close(); await observer.close();
}
