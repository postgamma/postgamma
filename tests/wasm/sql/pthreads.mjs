/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
import assert from 'node:assert/strict';
import {pathToFileURL} from 'node:url';
import {resolve} from 'node:path';
const {ownPthreads, installPthreadBroker} = await import(pathToFileURL(resolve(process.argv[2], 'runtime/pthreads.js')));
const {pthreadPoolSize} = await import(pathToFileURL(resolve(process.argv[2], 'protocol/limits.js')));
globalThis.ErrorEvent ??= class extends Event {
  constructor(type, init) {super(type, init); for (const key of ['message', 'filename', 'lineno', 'colno']) this[key] = init[key];}
};
const native = [], failures = [];
class NativeWorker {
  terminated = 0;
  constructor(url, options) {this.url = url; this.options = options; native.push(this);}
  postMessage(data, transfer) {
    const copy = structuredClone(data, {transfer});
    if (copy.port) {
      copy.port.postMessage({storageOwner: true}); copy.port.close();
    } else queueMicrotask(() => this.onmessage({data: copy, ports: []}));
  }
  terminate() {this.terminated++;}
}
const savedWorker = globalThis.Worker;
const moduleURL = new URL('https://example.invalid/assets/postgamma.mjs');
let stop, transferred, control, controlTerminations = 0;
function group() {
  globalThis.Worker = NativeWorker;
  control = {terminate() {controlTerminations++;}, postMessage(data, ports) {
    assert.equal(data.postgammaPthreadBroker, true); transferred = ports[0];
  }};
  stop = ownPthreads(control, moduleURL, error => failures.push(error));
  installPthreadBroker(transferred);
}
const wait = async test => {
  const deadline = performance.now() + 2000;
  while (!test()) {assert(performance.now() < deadline, 'Pthread broker progress'); await new Promise(resolve => setTimeout(resolve, 1));}
};
try {
  group();
  const worker = new Worker(moduleURL, {type: 'module', name: 'em-pthread-0'});
  await wait(() => native.length === 1);
  assert.equal(native[0].url.pathname, moduleURL.pathname);
  assert(new URLSearchParams(native[0].url.hash.slice(1)).get('postgammaPthread'));
  const assetURL = new URL(native[0].url); assetURL.hash = '';
  assert.equal(assetURL.href, moduleURL.href, 'Worker identity does not create another HTTP asset cache key');
  assert.equal(native[0].options.name, 'em-pthread-0');
  let received, listeners = 0;
  worker.addEventListener('message', () => listeners++);
  worker.onmessage = event => {received = event.data;};
  const bytes = new Uint8Array([1, 2, 3]);
  worker.postMessage({bytes: bytes.buffer}, [bytes.buffer]);
  assert.equal(bytes.byteLength, 0, 'Transfer detaches the control-side buffer');
  await wait(() => received);
  assert.deepEqual([...new Uint8Array(received.bytes)], [1, 2, 3]);
  assert.equal(listeners, 1, 'SDK properties and registered listeners both receive events');
  const direct = new MessageChannel(); let storageReceipt;
  direct.port1.onmessage = event => {storageReceipt = event.data;};
  worker.postMessage({port: direct.port2}, {transfer: [direct.port2]});
  await wait(() => storageReceipt);
  assert.deepEqual(storageReceipt, {storageOwner: true}, 'Transferred OPFS control ports remain direct channels');
  direct.port1.close();
  let error, prevented = false;
  worker.onerror = event => {event.preventDefault(); error = event;};
  native[0].onerror({message: 'native trap', filename: 'postgamma.mjs', lineno: 12, colno: 3,
    preventDefault() {prevented = true;}});
  await wait(() => error);
  assert(prevented && error.defaultPrevented);
  assert.equal(error.message, 'native trap'); assert.equal(error.lineno, 12);
  worker.terminate(); worker.terminate();
  await wait(() => native[0].terminated === 1);
  assert.deepEqual(failures, []);
  stop(); stop(); transferred.close();

  group();
  const pool = Array.from({length: pthreadPoolSize}, (_, id) => new Worker(moduleURL, {type: 'module', name: 'em-pthread-' + id}));
  await wait(() => native.length === pthreadPoolSize + 1);
  const excess = new Worker(moduleURL, {type: 'module', name: 'em-pthread-' + pthreadPoolSize});
  excess.onerror = () => {};
  await wait(() => failures.length === 1);
  assert.match(failures[0].message, /excessive/);
  const lateMessage = native.at(-1).onmessage;
  control.terminate(); stop(); transferred.close();
  const latePort = new MessageChannel(); let lateReply = false;
  latePort.port1.onmessage = () => {lateReply = true;};
  lateMessage({data: {stale: true}, ports: [latePort.port2]});
  latePort.port2.postMessage({stale: true});
  await new Promise(resolve => setTimeout(resolve, 5));
  assert.equal(lateReply, false, 'Queued messages from retired siblings close their transferred ports');
  latePort.port1.close(); latePort.port2.close();
  assert.equal(controlTerminations, 1, 'Direct control termination retires its sibling group');
  assert(native.every(worker => worker.terminated === 1), 'Group retirement kills all live children exactly once');
  for (const worker of [...pool, excess]) worker.terminate();

  group();
  const unexpected = new Worker('https://example.invalid/other.mjs', {type: 'module', name: 'em-pthread-0'});
  unexpected.onerror = () => {};
  await wait(() => failures.length === 2);
  assert.match(failures[1].message, /asset manifest/);
  assert.equal(native.length, pthreadPoolSize + 1, 'Undeclared worker URLs cannot allocate threads');
  unexpected.terminate(); stop(); transferred.close();
  console.log('Pthread ownership: transferable data, direct OPFS ports, error events, bounded allocation and group retirement passed');
} finally {stop?.(); transferred?.close(); globalThis.Worker = savedWorker;}
