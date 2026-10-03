/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
import assert from 'node:assert/strict';
import {pathToFileURL} from 'node:url';

const moduleUrl = pathToFileURL(process.argv[2]);
const {Database, Connection} = await import(moduleUrl);
const {Op, Reader, Writer, DatabaseError} = await import(new URL('../protocol/wire.js', moduleUrl));
const {Readiness} = await import(new URL('../protocol/readiness.js', moduleUrl));

function fixture({audit = false, malformed = false, stop = false, retry = false} = {}) {
  const calls = [];
  let closed = false, terminated = 0;
  const transport = {
    metrics: undefined, peakHostQueuedBytes: 31,
    closeEvents() {},
    async call(op) {
      calls.push(op);
      if (op === Op.close) {
        assert(!closed, 'A freed native handle must never be closed again');
        if (retry) {retry = false; throw new DatabaseError(5, [null, null, 'Close timed out']);}
        closed = true;
        const w = new Writer().u32(audit ? 1 : 0);
        if (!malformed) w.u32(0).u32(0).u32(0);
        return new Reader(w.finish());
      }
      assert.equal(op, Op.stop);
      assert(closed);
      transport.metrics = {runningWorkers: 0};
      if (stop) throw new Error('STOP failed');
      return new Reader(new ArrayBuffer(0));
    },
    terminate() {terminated++;},
  };
  // TypeScript's private constructor is accessed only by this transport unit test.
  const db = new Database(transport, {kind: 1, id: 1, generation: 1});
  return {db, calls, terminated: () => terminated};
}

for (const options of [{}, {audit: true}, {malformed: true}, {stop: true}, {audit: true, stop: true}]) {
  const f = fixture(options), work = f.db.close();
  assert.equal(f.db.close(), work, 'Concurrent close shares its completion');
  if (Object.keys(options).length) {
    const error = await work.then(() => assert.fail('Expected close failure'), error => error);
    if (options.audit && options.stop) {
      assert(error instanceof AggregateError);
      assert.deepEqual(error.errors.map(e => e.message), ['Native resources remain after close', 'STOP failed']);
    } else assert.match(error.message, /Native resources|Truncated wire|STOP failed/);
    assert.equal(f.db.close(), work, 'A terminal close failure retains its completion');
    await assert.rejects(f.db.close(), error);
  } else {
    const metrics = await work;
    assert.deepEqual(metrics, {runningWorkers: 0, resources: {locks: 0, shmem: 0, dsm: 0, pipes: 0}, peakHostQueuedBytes: 31});
    assert.equal(f.db.close(), work);
  }
  assert.deepEqual(f.calls, [Op.close, Op.stop]);
  assert.equal(f.terminated(), 1, 'Teardown runs even after audit or STOP failure');
}
const retry = fixture({retry: true});
await assert.rejects(retry.db.close(), /Close timed out/);
assert.equal(retry.terminated(), 0, 'Keep a live instance available for close retry');
await retry.db.close();
assert.deepEqual(retry.calls, [Op.close, Op.close, Op.stop]);
assert.equal(retry.terminated(), 1);
console.log('Close lifecycle: 6 success, failure and retry scenarios passed');

const handle = {kind: 2, id: 5, generation: 11};
for (const timeout of [false, true]) {
  const readiness = new Readiness();
  let attempts = 0, retired = 0;
  const transport = {readiness, unsubscribe() {}, async call(op, h) {
    assert.equal(op, Op.disconnect); assert.deepEqual(h, handle);
    attempts++;
    if (attempts === 1) {
      readiness.notify(h); // Native retry event arrives before the AGAIN reply.
      return new Reader(new Writer().u32(1).finish());
    }
    if (attempts === 2 && timeout) throw new DatabaseError(3, [null, null, 'Close timed out']);
    return new Reader(new Writer().u32(2).finish());
  }};
  const connection = new Connection(transport, handle, () => {retired++;});
  const closing = connection.close();
  assert.equal(connection.close(), closing);
  if (timeout) {
    await assert.rejects(closing, /Close timed out/);
    assert.equal(retired, 0, 'Timeout keeps the connection owned and retryable');
    assert.equal(readiness.version(handle), 1);
    await connection.close();
  } else await closing;
  assert.equal(attempts, timeout ? 3 : 2);
  assert.equal(retired, 1);
  assert.throws(() => readiness.version(handle), /Retired/);
  const reused = {...handle, generation: handle.generation + 1};
  readiness.track(reused); readiness.notify(handle);
  assert.equal(readiness.version(reused), 0, 'Late close edge does not reach the reused slot');
}

const readiness = new Readiness();
let submitted, retired = 0;
const submission = new Promise(resolve => {submitted = resolve;});
const connection = new Connection({readiness, unsubscribe() {}, async call() {
  submitted(); return new Reader(new Writer().u32(1).finish());
}}, handle, () => {retired++;});
const failedClose = assert.rejects(connection.close(), /Instance retired/);
await submission;
await new Promise(resolve => setImmediate(resolve));
readiness.fail(new Error('Instance retired'));
await failedClose;
assert.equal(retired, 0, 'Fatal cleanup is not mistaken for a successful native disconnect');
console.log('Disconnect: early readiness, timeout retry, stale generation and fatal wait passed');
