/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
import assert from 'node:assert/strict';
import {pathToFileURL} from 'node:url';
const {Namespace, encode} = await import(pathToFileURL(process.argv[2]));
const changes = [
  {op: 'create', node: {id: 2, parent: 1, name: 'dir', kind: 2, mode: 448}},
  {op: 'create', node: {id: 3, parent: 2, name: 'file', kind: 1, mode: 384}},
  {op: 'move', id: 2, parent: 1, name: 'renamed'},
  {op: 'remove', parent: 2, name: 'file'},
];
const records = changes.map((c, i) => encode(c, i + 1));
const bytes = Buffer.concat(records);
const whole = Namespace.replay(bytes);
assert.equal(whole.namespace.sequence, 4);
assert.equal(whole.namespace.lookup(1, 'renamed').id, 2);
assert.equal(whole.namespace.lookup(2, 'file'), undefined);
let boundary = 0;
for (let i = 0; i < records.length; i++) {
  for (let cut = 1; cut < records[i].length; cut++) {
    const replay = Namespace.replay(bytes.subarray(0, boundary + cut));
    assert.equal(replay.namespace.sequence, i);
    assert.equal(replay.validBytes, boundary);
  }
  boundary += records[i].length;
}
const corrupted = Buffer.from(bytes); corrupted[20] ^= 1;
assert.throws(() => Namespace.replay(corrupted), /interior/);
const last = Buffer.from(bytes); last[last.length - 1] ^= 1;
assert.equal(Namespace.replay(last).namespace.sequence, 3);
const ns = Namespace.replay(bytes.subarray(0, records[0].length)).namespace;
assert.throws(() => ns.apply({op: 'move', id: 2, parent: 2, name: 'cycle'}), /cycle/);
assert.throws(() => ns.apply(changes[0]), /creation/);
console.log('namespace: replay, every torn-tail length, interior corruption, identity and cycles passed');
