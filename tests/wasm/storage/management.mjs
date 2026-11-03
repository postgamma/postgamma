/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
import assert from 'node:assert/strict';
import {pathToFileURL} from 'node:url';
const {Database} = await import(pathToFileURL(process.argv[2]));
const entries = new Map([['complete', {kind: 'directory'}], ['incomplete', {kind: 'directory'}],
  ['not-a-database', {kind: 'file'}], ['invalid name', {kind: 'directory'}]]);
let rootExists = false, lockHeld = false, removing, deletion, rootCalls = 0, quotaFailure = false;
const directory = {
  async *entries() {yield* entries;},
  async removeEntry(name, options) {
    assert(lockHeld, 'Deletion holds the open/restore ownership lock');
    assert.deepEqual(options, {recursive: true});
    if (quotaFailure) throw new DOMException('injected I/O failure', 'UnknownError');
    if (!entries.has(name)) throw new DOMException('absent', 'NotFoundError');
    if (deletion) {removing(); await deletion; assert(lockHeld, 'Lock retained during asynchronous removal');}
    entries.delete(name);
  },
};
let persistRequests = 0;
const storage = {
  async getDirectory() {
    rootCalls++;
    return {async getDirectoryHandle(name, options) {
      assert.equal(name, 'postgamma'); assert.equal(options, undefined, 'Listing/deletion do not create storage');
      if (!rootExists) throw new DOMException('absent', 'NotFoundError');
      return directory;
    }};
  },
  async persisted() {return false;},
  async persist() {persistRequests++; return false;},
};
Object.defineProperty(globalThis, 'navigator', {configurable: true, value: {storage,
  locks: {async request(name, options, callback) {
    assert.match(name, /^postgamma:database:/); assert.equal(options.mode, 'exclusive');
    options.signal?.throwIfAborted(); assert(!lockHeld);
    lockHeld = true;
    try {return await callback({name});} finally {lockHeld = false;}
  }},
}});
assert.deepEqual(await Database.list(), []);
assert.equal(await Database.delete('absent'), false);
rootExists = true;
assert.deepEqual(await Database.list(), ['complete', 'incomplete']);
for (const name of ['', '../complete', 'invalid name', 'a'.repeat(65), null, undefined, 42, {}, ['complete']]) {
  const before = rootCalls;
  await assert.rejects(Database.delete(name), /Database name/);
  assert.equal(rootCalls, before, 'Invalid names never reach the filesystem');
}
const controller = new AbortController(); controller.abort(new Error('pre-aborted'));
await assert.rejects(Database.delete('complete', {signal: controller.signal}), /pre-aborted/);
let finish;
deletion = new Promise(resolve => {finish = resolve;});
const started = new Promise(resolve => {removing = resolve;});
const pending = Database.delete('incomplete');
await started; assert(lockHeld); finish();
assert.equal(await pending, true); assert(!lockHeld);
deletion = undefined;
quotaFailure = true;
await assert.rejects(Database.delete('complete'), /injected I\/O failure/);
assert(!lockHeld, 'Failed deletion also releases ownership'); quotaFailure = false;
assert.equal(await Database.delete('complete'), true);
assert.equal(await Database.delete('complete'), false);
assert.equal(await Database.storagePersistence(), 'best-effort'); assert.equal(persistRequests, 0);
assert.equal(await Database.storagePersistence({request: true}), 'best-effort'); assert.equal(persistRequests, 1);
storage.persist = async () => {persistRequests++; return true;};
assert.equal(await Database.storagePersistence({request: true}), 'persistent');
storage.persisted = async () => true;
assert.equal(await Database.storagePersistence({request: true}), 'persistent'); assert.equal(persistRequests, 2);
storage.persisted = async () => {throw new DOMException('denied', 'SecurityError');};
await assert.rejects(Database.storagePersistence(), /denied/);
delete storage.persisted;
assert.equal(await Database.storagePersistence({request: true}), 'unsupported');
await assert.rejects(Database.storagePersistence({request: 'yes'}), TypeError);
console.log('Storage management: lock lifetime, incomplete cleanup, errors, validation and explicit persistence passed');
