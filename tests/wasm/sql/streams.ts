/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
import type {Database} from '../src/api/database.js';
import {assert, scalar, errorState} from './helpers.js';
const utf8 = new TextEncoder();
const outcome = (work: Promise<unknown>) => work.then(() => undefined, error => error);
export async function streams(db: Database, checks: string[]) {
  const c = await db.connect('stream-copy');
  await c.query('CREATE TABLE stream_values(id int PRIMARY KEY, value text)');
  const csv = '1,"雪,one"\n2,"line\nbreak"\n3,"say ""hi"""\n';
  const input = new File([csv], 'input.csv', {type: 'text/csv'});
  assert((await c.copyFrom('COPY stream_values FROM STDIN (FORMAT csv)', input))[0].command === 'COPY 3', 'File COPY reports server completion');
  const bytes = utf8.encode('4,🐘\n'), byteStream = new ReadableStream<Uint8Array>({
    start(controller) {for (const byte of bytes) controller.enqueue(new Uint8Array([byte])); controller.close();},
  });
  await c.copyFrom('COPY stream_values FROM STDIN (FORMAT csv)', byteStream);
  assert(scalar(await c.query('SELECT value FROM stream_values WHERE id = 4')) === '🐘', 'COPY accepts UTF-8 split across source chunks');
  const output: Uint8Array[] = []; let closed = false;
  const target = new WritableStream<Uint8Array>({write(bytes) {output.push(bytes);}, close() {closed = true;}});
  assert((await c.copyTo('COPY (SELECT * FROM stream_values ORDER BY id) TO STDOUT (FORMAT csv)', target))[0].command === 'COPY 4',
    'Writable COPY reports server completion');
  assert(closed && await new Blob(output).text() === csv + '4,🐘\n', 'Writable COPY closes its sink and preserves CSV bytes');
  checks.push('copy-file-input', 'copy-chunked-unicode-input', 'copy-writable-output');

  await errorState(() => c.copyFrom('COPY stream_values FROM STDIN (FORMAT csv)', new Blob(['5,new\n1,duplicate\n'])), '23505');
  assert(scalar(await c.query('SELECT count(*) FROM stream_values')) === '4', 'COPY constraint error rolls back all input');
  const sourceError = new Error('source failed'); let emitted = false;
  const broken = new ReadableStream<Uint8Array>({pull(controller) {
    if (emitted) controller.error(sourceError); else {emitted = true; controller.enqueue(utf8.encode('6,partial\n'));}
  }});
  assert(await outcome(c.copyFrom('COPY stream_values FROM STDIN (FORMAT csv)', broken)) === sourceError, 'Source exception survives native cleanup');
  assert(scalar(await c.query('SELECT count(*) FROM stream_values')) === '4', 'Source exception rolls back COPY');
  const sinkError = new Error('sink failed');
  assert(await outcome(c.copyTo('COPY stream_values TO STDOUT', new WritableStream({write() {throw sinkError;}}))) === sinkError,
    'Sink exception survives native cleanup');
  assert(scalar(await c.query('SELECT 7')) === '7', 'Connection reusable after source and sink errors');
  checks.push('copy-constraint-rollback', 'copy-source-error-rollback', 'copy-sink-error-reuse');

  let sourceCanceled = false;
  const hanging = new ReadableStream<Uint8Array>({pull() {return new Promise(() => {});}, cancel() {sourceCanceled = true;}});
  assert((await outcome(c.copyFrom('COPY stream_values FROM STDIN', hanging, {timeoutMs: 500})))?.name === 'TimeoutError',
    'Deadline interrupts an unreadable application source');
  assert(sourceCanceled, 'Cancellation reaches the source');
  assert((await outcome(c.copyTo('COPY stream_values TO STDOUT', new WritableStream({write() {return new Promise(() => {});}}),
    {timeoutMs: 500})))?.name === 'TimeoutError', 'Deadline interrupts a stuck application sink');
  const controller = new AbortController(), cancel = outcome(c.copyFrom('COPY stream_values FROM STDIN',
    new ReadableStream(), {signal: controller.signal}));
  controller.abort(); assert((await cancel)?.name === 'AbortError', 'Queued COPY cancellation');
  assert(scalar(await c.query('SELECT count(*) FROM stream_values')) === '4' && (await db.telemetry()).requests === 0n,
    'COPY timeouts retire native requests before connection reuse');
  checks.push('copy-source-timeout', 'copy-sink-timeout', 'copy-queued-abort', 'copy-resource-release');
  await c.close();
}
