/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
import assert from 'node:assert/strict';
import {pathToFileURL} from 'node:url';
const {Readiness} = await import(pathToFileURL(process.argv[2]));
const {exchange} = await import(new URL('../bindings/sql.js', pathToFileURL(process.argv[2])));
const readiness = new Readiness(), signal = new AbortController();
const first = {kind: 3, id: 1, generation: 1}, second = {...first, id: 2};
readiness.track(first); readiness.track(second);
const version = readiness.version(first);
readiness.notify(first); // Completion arrived before the AGAIN response.
await readiness.wait(first, version, signal.signal);
let settled = false;
const pending = readiness.wait(first, readiness.version(first), signal.signal).then(() => {settled = true;});
readiness.notify(second);
await Promise.resolve(); await Promise.resolve();
assert.equal(settled, false, 'another connection must not wake this waiter');
readiness.notify(first); await pending;
const read = readiness.wait(first, readiness.version(first), new AbortController().signal);
const cancelSignal = new AbortController();
const cancel = readiness.wait(first, readiness.version(first), cancelSignal.signal);
cancelSignal.abort(new Error('cancel waiter stopped'));
await assert.rejects(cancel, /cancel waiter stopped/);
readiness.notify(first); await read;
const closed = readiness.wait(first, readiness.version(first), signal.signal);
readiness.retire(first);
await assert.rejects(closed, /Retired/);
const reused = {...first, generation: 2}; readiness.track(reused);
readiness.notify(first);
assert.equal(readiness.version(reused), 0, 'late event cannot reach a reused slot');
const canceled = readiness.wait(reused, 0, signal.signal);
signal.abort(new Error('closed by owner'));
await assert.rejects(canceled, /closed by owner/);
const failed = readiness.wait(second, readiness.version(second), new AbortController().signal);
readiness.fail(new Error('instance failed'));
await assert.rejects(failed, /instance failed/);

let state = 0, freed = 0, acked = 0;
const module = {
  wasmMemory: {buffer: new SharedArrayBuffer(256)},
  _postgamma_wasm_alloc: () => 64, _postgamma_wasm_free: () => {freed++;},
  _postgamma_wasm_submit: () => {state = 1; return 0;},
  _postgamma_wasm_poll: () => state, _postgamma_wasm_reply: () => 128,
  _postgamma_wasm_reply_size: () => 4,
  _postgamma_wasm_ack: () => {state = 0; acked++;},
};
const reply = exchange(module, new ArrayBuffer(12));
await Promise.resolve(); assert.equal(state, 1);
new Uint8Array(module.wasmMemory.buffer, 128, 4).set([1, 2, 3, 4]);
state = 2; module.postgammaBridgeReply();
const owned = await reply;
new Uint8Array(module.wasmMemory.buffer).fill(0);
assert.deepEqual([...new Uint8Array(owned)], [1, 2, 3, 4]);
assert.equal(acked, 1); assert.equal(freed, 0);
const abort = new AbortController();
const aborted = exchange(module, new ArrayBuffer(12), abort.signal);
abort.abort(); await assert.rejects(aborted, /Kernel instance failed/);
assert.equal(module.postgammaBridgeReply, undefined);
assert.equal(freed, 0, 'submitted memory belongs to the discarded instance');
console.log('readiness: early edge, independent waiters, retirement, abort, failure and owned replies passed');
