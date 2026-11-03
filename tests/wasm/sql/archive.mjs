/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
import assert from 'node:assert/strict';
import {pathToFileURL} from 'node:url';
const url = pathToFileURL(process.argv[2]);
const {Database} = await import(url);
const {Op, Reader, Writer} = await import(new URL('../protocol/wire.js', url));
const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));
function fixture() {
  const calls = []; let stops = 0, completeRead, beganRead;
  const reading = new Promise(resolve => {beganRead = resolve;});
  const pending = new Promise(resolve => {completeRead = resolve;});
  const transport = {metrics: {}, peakHostQueuedBytes: 0, closeEvents() {}, terminate() {stops++;},
    async call(op) {
      calls.push(op);
      if (op === Op.close) return new Reader(new Writer().u32(0).u32(0).u32(0).u32(0).finish());
      if (op === Op.stop) return new Reader(new ArrayBuffer(0));
      assert.equal(op, Op.archiveRead); beganRead(); await pending;
      return new Reader(new Uint8Array(4).buffer); // EOF
    }};
  const db = new Database(transport, {kind: 1, id: 1, generation: 1});
  return {db, calls, stops: () => stops, reading, completeRead};
}
{
  const f = fixture();
  await assert.rejects(f.db.dumpDataDir({idleTimeoutMs: -1}), /idleTimeoutMs/);
  assert.deepEqual(f.calls, [], 'Invalid option must not close the source');
  const stream = await f.db.dumpDataDir({idleTimeoutMs: 20});
  await sleep(50);
  await assert.rejects(stream.getReader().read(), error => error.name === 'TimeoutError');
  await f.db.close(); assert.equal(f.stops(), 1);
  assert.deepEqual(f.calls, [Op.close, Op.stop], 'Unread timeout releases storage without reading the archive');
}
{
  const f = fixture(), stream = await f.db.dumpDataDir();
  await sleep(30); assert.equal(f.stops(), 0, 'Default preserves backpressure without a deadline');
  await stream.cancel(); await f.db.close(); assert.equal(f.stops(), 1);
}
{
  const f = fixture(), stream = await f.db.dumpDataDir({idleTimeoutMs: 20}), reader = stream.getReader();
  assert.equal((await reader.read()).done, false); // header
  const last = reader.read(); await f.reading; await sleep(50);
  assert.equal(f.stops(), 0, 'A pending pull is active work, not consumer idleness');
  f.completeRead(); assert.equal((await last).done, false);
  assert.equal((await reader.read()).done, true);
  await sleep(30); await f.db.close(); assert.equal(f.stops(), 1);
}
{
  const f = fixture(), controller = new AbortController();
  const stream = await f.db.dumpDataDir({idleTimeoutMs: 20, signal: controller.signal});
  controller.abort(new Error('cancel export'));
  await assert.rejects(stream.getReader().read(), /cancel export/);
  await f.db.close(); await sleep(30); assert.equal(f.stops(), 1);
}
console.log('Archive idle deadlines: unread, disabled, active pull, EOF and abort cleanup passed');
