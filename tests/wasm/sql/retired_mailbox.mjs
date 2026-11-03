/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import {pathToFileURL} from 'node:url';
const source = readFileSync(process.argv[2], 'utf8');
const postset = source.match(/\$checkMailbox__postset:\s*`([^`]+)`/s)?.[1];
assert(postset, 'SDK mailbox retirement adapter is installed');
for (const flag of ['postgammaNativeRetired', 'postgammaStorageRetired']) {
  const global = {}; let calls = 0;
  const dispatch = new Function('globalThis', 'checkMailbox', 'ENVIRONMENT_IS_PTHREAD', postset + ';return checkMailbox;')(global, () => {calls++;}, false);
  dispatch(); assert.equal(calls, 1, 'Live mailbox keeps the SDK implementation');
  // Simulate a callback already queued before the native owners were killed.
  const queued = dispatch; global[flag] = true;
  queued(); assert.equal(calls, 1, 'Retired mailbox never re-enters native mutexes');
}
{
  let listener, calls = 0; const events = [];
  const realm = {addEventListener(_kind, callback) {listener = callback;}, close() {events.push('scope-close');}};
  const dispatch = new Function('globalThis', 'checkMailbox', 'ENVIRONMENT_IS_PTHREAD', postset + ';return checkMailbox;')(realm, () => {calls++;}, true);
  dispatch(); assert.equal(calls, 1);
  listener({data: {postgammaRetirePthread: true}, ports: [{
    postMessage(data) {assert.equal(data.retired, true); assert(realm.postgammaNativeRetired); events.push('ack');},
    close() {events.push('port-close');},
  }]});
  dispatch(); assert.equal(calls, 1, 'Retired failed pthread cannot enter its native mailbox');
  assert.deepEqual(events, ['ack', 'port-close', 'scope-close']);
}
// Execute the actual EM_JS owner adapter: quiescence must precede caller
// termination, and the owner's scope closes only after the release reply.
const storage = readFileSync(new URL('storage.cpp', pathToFileURL(process.argv[2])), 'utf8');
const start = storage.indexOf('EM_JS(void, postgamma_storage_listen,');
assert(start >= 0);
const body = storage.slice(storage.indexOf('{', start) + 1, storage.indexOf('\n});', start));
for (const fails of [false, true]) {
  const events = []; let listen, channel, finish, nativeCalls = 0;
  const pending = new Promise(resolve => {finish = resolve;});
  const realm = {
    addEventListener(_kind, callback) {listen = callback;},
    close() {events.push('scope-close');},
  };
  const Channel = class {
    constructor() {channel = this;}
    close() {events.push('channel-close');}
  };
  new Function('globalThis', 'UTF8ToString', 'BroadcastChannel', '_emscripten_proxy_execute_queue', 'name', 'queue', 'workers', body)
    (realm, () => 'fixture', Channel, () => {nativeCalls++;}, 1, 2, 13);
  const queued = channel.onmessage;
  queued(); assert.equal(nativeCalls, 1, 'Live owner receives native proxy work');
  realm.postgammaStoragePending.set(pending, {operation: 1});
  realm.postgammaStore = {
    interrupt() {events.push('interrupt');},
    async discard() {
      assert(!events.includes('scope-close'), 'Owner survives while physical handles close');
      if (fails) throw new Error('forced close failure');
      events.push('physical-close'); return 3;
    },
  };
  const port = {
    postMessage(data) {
      if (data.quiesced) {assert(realm.postgammaStorageRetired); events.push('quiesced');}
      else {
        assert.equal(data.released, !fails);
        if (!fails) {assert.equal(data.handles, 3); assert(events.includes('physical-close'));}
        events.push('release-reply');
      }
    },
    close() {events.push('port-close');},
  };
  listen({data: {postgammaStorageControl: true}, ports: [port]});
  const closing = port.onmessage({data: {discard: true}});
  assert(events.includes('quiesced') && !events.includes('release-reply'));
  queued(); assert.equal(nativeCalls, 1, 'Queued proxy work cannot re-enter the retired native module');
  finish(); await closing;
  assert.deepEqual(events.slice(-3), ['release-reply', 'port-close', 'scope-close']);
}

// BroadcastChannel also targets channels without message listeners. A native
// caller blocked in Atomics.wait cannot dispatch those browser tasks, so ensure
// notification fanout never adds a message to any other caller's queue.
{
  const channels = new Set(); let nativeCalls = 0;
  class Channel {
    pending = [];
    constructor(name) {this.name = name; channels.add(this);}
    postMessage(data) {
      for (const channel of channels)
        if (channel !== this && channel.name === this.name) channel.pending.push(data);
    }
    close() {channels.delete(this);}
  }
  const owner = {addEventListener() {}};
  new Function('globalThis', 'UTF8ToString', 'BroadcastChannel', '_emscripten_proxy_execute_queue', 'name', 'queue', 'workers', body)
    (owner, () => 'fixture', Channel, () => {nativeCalls++;}, 1, 2, 13);
  const receivers = [...channels];
  const begin = storage.indexOf('EM_JS(void, postgamma_storage_notify,');
  const notify = new Function('globalThis', 'UTF8ToString', 'BroadcastChannel', 'ENVIRONMENT_IS_PTHREAD', 'name',
    storage.slice(storage.indexOf('{', begin) + 1, storage.indexOf('\n});', begin)));
  const callers = [{}, ...Array.from({length: 13}, (_, index) => ({name: 'em-pthread-' + (index + 1)}))];
  for (let round = 0; round < 1000; round++)
    for (const caller of callers) notify(caller, () => 'fixture', Channel, !!caller.name, 1);
  for (const caller of callers)
    assert.equal(caller.postgammaIOSender.pending.length, 0, 'Blocked caller receives no I/O wakeups');
  for (const channel of receivers) {
    assert.equal(channel.pending.length, 1000, 'Every queued notification reaches its I/O receiver');
    for (const message of channel.pending) channel.onmessage({data: message});
  }
  assert.equal(nativeCalls, 14000);
  // Pool reuse retains the same Worker identity and receiver, while changing
  // database instances closes and replaces that Worker's old sender.
  const caller = callers[1], old = caller.postgammaIOSender;
  notify(caller, () => 'next-instance', Channel, true, 1);
  assert(!channels.has(old)); assert.notEqual(caller.postgammaIOSender, old);
}
