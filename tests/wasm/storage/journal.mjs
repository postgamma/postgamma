/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
import assert from 'node:assert/strict';
import {pathToFileURL} from 'node:url';
import {resolve} from 'node:path';
const source = name => pathToFileURL(resolve(process.argv[2], name + '.js'));
const {Journal, journalNames} = await import(source('journal'));
const {Namespace, encode, journalLimit} = await import(source('namespace'));
const {snapshot, restore, slotLimit} = await import(source('checkpoint'));
const {Store} = await import(source('opfs'));

// Ordinary I/O and unrelated injection points must not walk parent paths.
const store = new Store();
store.path = () => {throw new Error('Unexpected namespace path traversal');};
assert.equal(await store.point('write', 2), '');
store.fault = {operation: 'flush-before', path: '/data', action: 'pause', remaining: 1};
assert.equal(await store.point('write', 2), '');

// Writes and flushes are distinct. A crash drops every unflushed change and
// releases handles without executing product close/flush code.
class File {
  bytes = new Uint8Array(256);
  durable = new Uint8Array(256);
  size = 0;
  persistedSize = 0;
  dirty = Infinity;
  end = 0;
  opened = false;
  constructor(root, name) {this.root = root; this.name = name;}
  grow(size) {
    if (size <= this.bytes.length) return;
    let capacity = this.bytes.length;
    while (capacity < size) capacity *= 2;
    const bytes = new Uint8Array(capacity), durable = new Uint8Array(capacity);
    bytes.set(this.bytes); durable.set(this.durable);
    this.bytes = bytes; this.durable = durable;
  }
  async createSyncAccessHandle() {
    if (this.opened) throw new DOMException('already open', 'NoModificationAllowedError');
    this.opened = true;
    return {
      getSize: () => this.size,
      read: (data, {at}) => {
        const n = Math.min(data.length, this.size - at);
        data.set(this.bytes.subarray(at, at + n)); return n;
      },
      write: (data, {at}) => {
        this.root.operation('write', this.name);
        const n = Math.min(data.length, this.root.writeSize);
        this.grow(at + n);
        this.bytes.set(data.subarray(0, n), at);
        this.size = Math.max(this.size, at + n);
        this.dirty = Math.min(this.dirty, at); this.end = Math.max(this.end, at + n);
        return n;
      },
      truncate: size => {
        this.root.operation('truncate', this.name);
        this.grow(size);
        if (size > this.size) this.bytes.fill(0, this.size, size);
        this.dirty = Math.min(this.dirty, this.size, size);
        this.end = Math.max(this.end, size);
        this.size = size;
      },
      flush: () => {
        this.root.operation('flush', this.name);
        if (this.dirty < this.end) this.durable.set(this.bytes.subarray(this.dirty, this.end), this.dirty);
        this.persistedSize = this.size;
        this.dirty = Infinity; this.end = 0;
      },
      close: () => {this.opened = false;},
    };
  }
  crash() {
    this.bytes.set(this.durable); this.size = this.persistedSize;
    this.dirty = Infinity; this.end = 0; this.opened = false;
  }
}
class Directory {
  files = new Map();
  operations = 0;
  trace = null;
  failAt = Infinity;
  writeSize = Infinity;
  operation(kind) {
    this.trace?.push(kind);
    if (++this.operations === this.failAt) throw new DOMException('injected I/O failure', 'UnknownError');
  }
  async getFileHandle(name, {create = false} = {}) {
    if (!this.files.has(name)) {
      if (!create) throw new DOMException('missing', 'NotFoundError');
      this.files.set(name, new File(this, name));
    }
    return this.files.get(name);
  }
  async *entries() {yield* this.files.entries();}
  crash() {this.failAt = Infinity; for (const file of this.files.values()) file.crash();}
  bytes(name) {const file = this.files.get(name); return file.bytes.slice(0, file.size);}
}
const create = (journal, name, parent = 1, kind = 2) => journal.append({op: 'create',
  node: {id: journal.namespace.nextId, parent, name, kind, mode: 448}});
const remove = (journal, name, parent = 1) => journal.append({op: 'remove', parent, name});
const state = journal => ({nextId: journal.namespace.nextId,
  nodes: [...journal.namespace.nodes.values()].map(node => ({...node})).sort((a, b) => a.id - b.id)});
const hooks = point => ({point, partial: async () => {throw new Error('interrupted tail');}});

let root = new Directory(), journal = await Journal.open(root);
await create(journal, 'old');
await create(journal, 'later');
await journal.append({op: 'move', id: 2, parent: 3, name: 'child'});
await create(journal, 'gone'); await remove(journal, 'gone');
const before = state(journal);
const checkpoint = snapshot(journal.namespace, 9), decoded = restore(checkpoint);
assert.deepEqual(state(decoded), before);
assert.equal(decoded.generation, 9);
const maximum = Namespace.restore([{id: 1, parent: 0, name: '', kind: 2, mode: 448},
  ...Array.from({length: 16383}, (_, i) => ({id: i + 2, parent: 1,
    name: String(i).padStart(6, '0') + '\u0001'.repeat(249), kind: 2, mode: 448}))], 16385);
const largest = snapshot(maximum, 1);
assert(largest.length < slotLimit - 4096);
assert.equal(restore(largest).namespace.nodes.size, 16384);
for (let length = 0; length < checkpoint.length; length++) assert.throws(() => restore(checkpoint.subarray(0, length)));
const corrupt = checkpoint.slice(); corrupt[corrupt.length - 1] ^= 1;
assert.throws(() => restore(corrupt), /Corrupt/);
root.trace = [];
await journal.checkpoint();
assert.equal(root.trace.filter(op => op === 'write').length, 3, 'One snapshot write plus two retirement markers');
assert.equal(root.trace.filter(op => op === 'flush').length, 5, 'Only snapshot and retirement durability barriers');
root.trace = null;
journal.close();
journal = await Journal.open(root);
assert.deepEqual(state(journal), before);
await create(journal, 'next');
assert.equal(journal.namespace.lookup(1, 'next').id, before.nextId);
journal.close();

// Lifetime churn exceeded the old 16 MiB limit with only one live node.
root = new Directory(); journal = await Journal.open(root);
for (let i = 0; i < 120000; i++) {await create(journal, 'tmp'); await remove(journal, 'tmp');}
assert.equal(journal.namespace.nodes.size, 1);
assert(journal.metrics.compactions >= 3);
assert(journal.metrics.peakJournalBytes <= journalLimit);
const nextId = journal.namespace.nextId;
journal.close(); journal = await Journal.open(root);
await create(journal, 'after-reopen');
assert.equal(journal.namespace.lookup(1, 'after-reopen').id, nextId);
assert(journal.metrics.journalBytes <= slotLimit + 32);
journal.close();

// Every snapshot publication boundary preserves the same namespace and ID
// watermark, including a flushed half-snapshot and two complete generations.
const points = ['checkpoint-before', 'checkpoint-prepared', 'checkpoint-partial',
  'checkpoint-written', 'checkpoint-flushed', 'checkpoint-retired', 'checkpoint-published'];
for (const point of points) {
  root = new Directory(); let armed = false;
  const injection = hooks(async operation => {
    if (armed && operation === point) throw new Error('interrupted ' + point);
    return '';
  });
  if (point === 'checkpoint-partial') injection.writeCheckpoint = async (bytes, write, flush) => {
    if (!armed) {write(bytes, 0); return;}
    write(bytes.subarray(0, Math.floor(bytes.length / 2)), 0); flush();
    throw new Error('interrupted checkpoint-partial');
  };
  journal = await Journal.open(root, injection);
  await create(journal, 'kept'); await create(journal, 'deleted'); await remove(journal, 'deleted');
  const expected = state(journal); armed = true;
  await assert.rejects(journal.checkpoint(), /interrupted/);
  root.crash(); journal = await Journal.open(root);
  assert.deepEqual(state(journal), expected, point);
  await create(journal, 'continued');
  assert.equal(journal.namespace.lookup(1, 'continued').id, expected.nextId);
  journal.close();
}

// Fail each physical write/flush/truncate, not just the named logical gates.
let faultChecks = 0;
for (let fail = 1; ; fail++) {
  root = new Directory(); journal = await Journal.open(root);
  await create(journal, 'kept'); const expected = state(journal);
  const start = root.operations; root.failAt = start + fail;
  let failed = false;
  try {await journal.checkpoint();} catch {failed = true;}
  const count = root.operations - start;
  root.crash(); journal = await Journal.open(root);
  assert.deepEqual(state(journal), expected, 'physical failure ' + fail);
  journal.close();
  if (!failed) {assert(fail > count); break;}
  faultChecks++;
}

// A crash before the first complete checkpoint can retry an empty store.
for (let fail = 1; fail <= faultChecks; fail++) {
  root = new Directory(); root.failAt = fail;
  try {journal = await Journal.open(root); journal.close();} catch {}
  root.crash(); journal = await Journal.open(root);
  assert.equal(journal.namespace.nodes.size, 1); journal.close();
}

// Migrating a legacy database keeps live identities and rejects old readers.
root = new Directory();
const legacyFile = await root.getFileHandle(journalNames[0], {create: true});
let access = await legacyFile.createSyncAccessHandle();
const legacy = encode({op: 'create', node: {id: 19, parent: 1, name: 'legacy', kind: 2, mode: 448}}, 1);
access.write(legacy, {at: 0}); access.flush(); access.close();
journal = await Journal.open(root);
assert.equal(journal.namespace.lookup(1, 'legacy').id, 19);
assert.equal(journal.namespace.nextId, 20);
assert.throws(() => Namespace.replay(root.bytes(journalNames[0])), /Corrupt/);
journal.close();

// An old client may append after an interrupted migration. Its still-valid
// legacy log remains authoritative until it has actually been retired.
root = new Directory();
access = await (await root.getFileHandle(journalNames[0], {create: true})).createSyncAccessHandle();
access.write(legacy, {at: 0}); access.flush(); access.close();
await assert.rejects(Journal.open(root, hooks(async point => {
  if (point === 'checkpoint-flushed') throw new Error('migration interrupted');
  return '';
})), /migration interrupted/);
root.crash();
access = await root.files.get(journalNames[0]).createSyncAccessHandle();
access.write(encode({op: 'create', node: {id: 20, parent: 1, name: 'legacy-new', kind: 2, mode: 448}}, 2), {at: legacy.length});
access.flush(); access.close();
journal = await Journal.open(root);
assert.equal(journal.namespace.lookup(1, 'legacy-new').id, 20);
journal.close();

// A corrupt active snapshot cannot silently fall back after old retirement.
root = new Directory(); journal = await Journal.open(root);
await create(journal, 'kept'); await journal.checkpoint(); journal.close();
const active = [...root.files.values()].find(file => {try {restore(file.bytes.subarray(0, file.size)); return true;} catch {return false;}});
active.bytes[35] ^= 1;
await assert.rejects(Journal.open(root), /No complete namespace checkpoint/);
assert([...root.files.values()].every(file => !file.opened));

// Short writes make progress; zero writes fail without hanging.
root = new Directory(); root.writeSize = 7;
journal = await Journal.open(root); await create(journal, 'short'); journal.close();
journal = await Journal.open(root); assert(journal.namespace.lookup(1, 'short'));
root.writeSize = 0;
await assert.rejects(create(journal, 'zero'), /invalid progress/); journal.close();
console.log(JSON.stringify({status: 'passed', churnCycles: 120000, snapshotCrashBoundaries: points.length,
  physicalFailureBoundaries: faultChecks, lifetimeBoundBytes: journalLimit, legacyMigration: true}));
