/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
import assert from 'node:assert/strict';
import {pathToFileURL} from 'node:url';
const {SerialQueue} = await import(pathToFileURL(process.argv[2]));
const deferred = () => {let resolve; const promise = new Promise(r => {resolve = r;}); return {promise, resolve};};
const queue = new SerialQueue(), hold = deferred();
let started = false, released = 0, canceledActions = 0;
const first = queue.run(async () => {started = true; return hold.promise;});
await Promise.resolve(); await Promise.resolve();
assert(started);
for (let i = 0; i < 1000; i++) {
  const abort = new AbortController(), reason = new Error('queued cancellation');
  const pending = queue.run(async () => {canceledActions++;}, {signal: abort.signal}, () => released++);
  abort.abort(reason);
  await assert.rejects(pending, error => error === reason);
  assert.equal(queue.size, 1, 'Canceled jobs are removed while the earlier operation remains active');
}
assert.equal(released, 1000); assert.equal(canceledActions, 0);
await assert.rejects(queue.run(async () => {canceledActions++;}, {timeoutMs: 0}), {name: 'TimeoutError'});
assert.equal(canceledActions, 0);
const next = queue.run(async () => 'after');
hold.resolve('first'); assert.equal(await first, 'first'); assert.equal(await next, 'after');
await queue.idle(); assert.equal(queue.size, 0);
const order = [];
await Promise.all([queue.run(async () => {order.push(1); throw new Error('failure');}).catch(() => {}),
  queue.run(async () => {order.push(2);})]);
assert.deepEqual(order, [1, 2]);
const gate = deferred(), ready = deferred(), controller = new AbortController();
let settled = false;
const active = queue.run(async signal => {
  ready.resolve();
  await new Promise(resolve => signal.addEventListener('abort', resolve, {once: true}));
  await gate.promise; throw signal.reason;
}, {signal: controller.signal}).catch(error => {settled = true; return error;});
await ready.promise; const reason = new Error('active cancellation'); controller.abort(reason);
await Promise.resolve(); assert.equal(settled, false, 'Cancellation waits for active cleanup');
gate.resolve(); assert.equal(await active, reason);
queue.stop(reason);
await assert.rejects(queue.run(async () => 1), error => error === reason);
assert.equal(queue.size, 0);
console.log('queue: ordering, removable canceled jobs, deadlines and cleanup-before-rejection passed');
