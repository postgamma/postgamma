/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import {resolve} from 'node:path';
import {pathToFileURL} from 'node:url';
import {runInNewContext} from 'node:vm';
import {WorkerFixture, waitFor} from './worker-fixture.mjs';
const source = resolve(process.argv[2], 'runtime/messages.js');
const {ownWorkerMailbox} = await import(pathToFileURL(source));
globalThis.ErrorEvent ??= class extends Event {
  constructor(type, init) {super(type, init); for (const key of ['message', 'filename', 'lineno', 'colno', 'error']) this[key] = init[key];}
};
const workers = [], channels = [];
try {
  const worker = new WorkerFixture(); workers.push(worker);
  const ready = ownWorkerMailbox(worker);
  let received, ownerEvents = 0;
  worker.onmessage = event => {received = event.data; ownerEvents++;};
  worker.receive = (data, ports) => worker.emit({data, ports}, ports);
  await ready;
  assert.equal(ownerEvents, 0, 'Private bootstrap does not reach command callbacks');
  const bytes = new Uint8Array([7, 8, 9]);
  worker.postMessage({bytes: bytes.buffer}, {transfer: [bytes.buffer]});
  assert.equal(bytes.byteLength, 0, 'Owner transfer detaches immediately');
  await waitFor(() => received);
  assert.deepEqual([...new Uint8Array(received.data.bytes)], [7, 8, 9]);
  const direct = new MessageChannel(); channels.push(direct);
  received = undefined; worker.postMessage({direct: direct.port2}, [direct.port2]);
  await waitFor(() => received);
  assert.equal(received.ports.length, 1, 'Transferred port appears in the Worker event');
  let reply; direct.port1.onmessage = event => {reply = event.data;};
  received.data.direct.postMessage('direct'); await waitFor(() => reply);
  assert.equal(reply, 'direct'); received.data.direct.close();
  assert.equal(worker.nativePosts, 0);
  worker.terminate(); worker.terminate(); assert.equal(worker.terminated, 1);
  worker.postMessage('after termination'); assert.equal(worker.nativePosts, 0);

  for (const kind of ['terminated', 'error', 'invalid', 'unreadable', 'timeout']) {
    const worker = new WorkerFixture(false); workers.push(worker);
    const setTimer = globalThis.setTimeout, clearTimer = globalThis.clearTimeout;
    let deadline;
    if (kind === 'timeout') {
      globalThis.setTimeout = (callback, ms) => {assert.equal(ms, 30000); deadline = callback; return 1;};
      globalThis.clearTimeout = () => {};
    }
    try {
      const failure = assert.rejects(ownWorkerMailbox(worker), /mailbox|handshake/);
      if (kind === 'terminated') worker.terminate();
      else if (kind === 'timeout') deadline();
      else if (kind === 'error') {
        const event = new Event('error', {cancelable: true}); event.message = 'module import failed';
        worker.dispatchEvent(event); assert(event.defaultPrevented);
      } else worker.dispatchEvent(new MessageEvent(kind === 'invalid' ? 'message' : 'messageerror', {data: {unexpected: true}}));
      await failure; assert.equal(worker.terminated, 1); assert.equal(worker.nativePosts, 0);
      assert([...worker.captures.values()].every(listeners => !listeners.size), 'Bootstrap failure removes every listener');
    } finally {globalThis.setTimeout = setTimer; globalThis.clearTimeout = clearTimer;}
  }

  // Execute the actual child installer in a dedicated Worker-like global scope,
  // then exchange real ports with the owner. Installing twice must send once.
  class DedicatedWorkerGlobalScope extends EventTarget {
    // vm context globals are proxies with another prototype; brand this one.
    static [Symbol.hasInstance](value) {return value?.DedicatedWorkerGlobalScope === this;}
  }
  const scope = new DedicatedWorkerGlobalScope(); let bootstraps = 0, childData;
  Object.assign(scope, {DedicatedWorkerGlobalScope, name: 'em-pthread-0',
    MessageChannel: class extends MessageChannel {constructor() {super(); channels.push(this);}},
    MessageEvent, setTimeout, clearTimeout});
  const childOwner = new WorkerFixture(false); workers.push(childOwner);
  // The child transfers its mailbox to this owner, independently of the fixture.
  scope.postMessage = (data, ports) => {
    bootstraps++; assert.equal(data.postgammaWorkerMailbox, true);
    childOwner.dispatchEvent(new MessageEvent('message', {data, ports}));
  };
  const childReady = ownWorkerMailbox(childOwner);
  runInNewContext(readFileSync(source, 'utf8').replaceAll('export function ', 'function ') +
    '\ninstallWorkerMailbox(); installWorkerMailbox();', scope);
  await childReady; assert.equal(bootstraps, 1);
  scope.addEventListener('message', event => {
    childData = event.data;
    scope.postMessage({childReply: event.data}, [...event.ports]);
  });
  let childReply; childOwner.onmessage = event => {childReply = event.data;};
  childOwner.postMessage({command: 42});
  await waitFor(() => childReply);
  assert.deepEqual(childData, {command: 42}); assert.deepEqual(childReply, {childReply: {command: 42}});
  assert.equal(childOwner.nativePosts, 0);
  const original = new RangeError('Maximum call stack size exceeded.');
  original.stack = 'RangeError: Maximum call stack size exceeded.\n    at nativeStartup (postgamma.mjs:12:3)';
  let childError;
  childOwner.onerror = event => {event.preventDefault(); childError = event;};
  const exception = new ErrorEvent('error', {cancelable: true, message: original.message, error: original,
    filename: 'postgamma.mjs', lineno: 12, colno: 3});
  scope.dispatchEvent(exception);
  await waitFor(() => childError);
  assert(exception.defaultPrevented, 'A forwarded exception does not also propagate as a browser error');
  assert.equal(childError.error.name, 'RangeError');
  assert.equal(childError.error.stack, original.stack, 'The original pthread stack survives its Worker boundary');
  assert.equal(childError.filename, 'postgamma.mjs');
  assert.equal(childError.lineno, 12);
  assert.deepEqual(childReply, {childReply: {command: 42}}, 'Error diagnostics do not reach the SDK command listener');
  const noStack = new ErrorEvent('error', {cancelable: true, message: 'Module import failed'});
  scope.dispatchEvent(noStack);
  assert(!noStack.defaultPrevented, 'Unavailable stack leaves native error propagation intact');
  // Worker scope destruction owns the child endpoint. Close it explicitly in
  // this in-process fixture to give Node the same scope lifetime.
  scope.postMessage = () => {};
  childOwner.terminate();
  console.log('Worker mailboxes: child bootstrap, zero native owner posts, transferable data/ports and bounded failure cleanup passed');
} finally {for (const worker of workers) worker.terminate(); for (const channel of channels) {channel.port1.close(); channel.port2.close();}}
