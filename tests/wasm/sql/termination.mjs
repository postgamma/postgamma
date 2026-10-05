/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
import assert from 'node:assert/strict';
import {cp, mkdtemp, rm, writeFile} from 'node:fs/promises';
import {tmpdir} from 'node:os';
import {join, resolve} from 'node:path';
import {pathToFileURL} from 'node:url';

const directory = await mkdtemp(join(tmpdir(), 'pgm-termination-'));
let port, unblock, timer;
try {
  await cp(resolve(process.argv[2]), directory, {recursive: true});
  await writeFile(join(directory, 'package.json'), '{"type":"module"}');
  const {tick: actualTick} = await import(pathToFileURL(join(directory, 'bindings/sql.js')) + '?actual-tick');
  for (const [name, content] of Object.entries({
    'loader/kernel.js': 'export const loadKernel = async () => globalThis.fixture.module;',
    'bindings/sql.js': 'export const exchange = (...args) => globalThis.fixture.exchange(...args); export const tick = () => globalThis.fixture.tick();',
    'storage/ownership.js': 'export const acquire = async () => globalThis.fixture.release;',
  })) await writeFile(join(directory, name), content);
  const {Writer, Op, VERSION} = await import(pathToFileURL(join(directory, 'protocol/wire.js')));
  for (const failure of ['startup', 'pthread-owner', 'pthread-driver', 'pthread-timeout']) {
    globalThis.postgammaNativeRetired = false;
    const deadline = new Promise((_, reject) => {timer = setTimeout(() => reject(new Error('No fatal cleanup acknowledgement')), 7000);});
    let began, delivered;
    const cleaning = new Promise(resolve => {began = resolve;});
    const completed = new Promise(resolve => {delivered = resolve;});
    const pendingIO = new Promise(resolve => {unblock = resolve;});
    const messages = [];
    let physicalHandle = true, released = false, terminatedEarly = false, terminated = 0;
    let quiesced = false, nativeStoppedEarly = false, nativeStops = 0;
    let ownerStopped = false;
    let pthreadErrorHandled = false;
    let driverRetired = false, driverForced = false;
    const retiredSdkMessage = () => {throw new Error('SDK callback re-entered a discarded module');};
    const owner = {
      onmessage: retiredSdkMessage,
      postMessage(data, ports) {
        assert.equal(data.postgammaStorageControl, true);
        port = ports[0];
        port.onmessage = async () => {
          quiesced = true;
          began(); await pendingIO;
          physicalHandle = false;
          port.postMessage({released: true, handles: 3});
          port.close();
          setTimeout(() => {ownerStopped = true;}, 0);
        };
      },
      terminate() {
        terminated++; terminatedEarly ||= physicalHandle;
        setTimeout(() => {ownerStopped = true;}, 0);
      },
    };
    const driver = {
      onmessage: retiredSdkMessage,
      postMessage(data, ports) {
        assert.equal(data.postgammaRetirePthread, true);
        assert(quiesced, 'The faulting driver retires after I/O native callbacks quiesce');
        if (failure === 'pthread-timeout') {ports[0].close(); return;}
        setTimeout(() => {
          driverRetired = true;
          ports[0].postMessage({retired: true});
          ports[0].close();
        }, 0);
      },
      terminate() {driverForced = true;},
    };
    const module = {
      wasmMemory: {buffer: new ArrayBuffer(1024)},
      PThread: {pthreads: {1: owner, 2: {onmessage: retiredSdkMessage, terminate() {nativeStops++; nativeStoppedEarly ||= !quiesced;}}}, unusedWorkers: [driver], terminateAllThreads() {
        assert.equal(globalThis.postgammaNativeRetired, true, 'Control mailbox retires before native callers');
        for (const worker of Object.values(this.pthreads)) worker.terminate();
        for (const worker of this.unusedWorkers) worker.terminate();
        this.pthreads = {};
        this.unusedWorkers = [];
      }},
      _postgamma_wasm_start() {
        this.postgammaStorageOwner?.(owner);
        if (failure.startsWith('pthread')) (failure === 'pthread-owner' ? owner : driver).onerror({error: new Error('pthread failed'),
          preventDefault() {pthreadErrorHandled = true;}});
        return 0;
      },
      _postgamma_wasm_wake_count: () => 0,
      _postgamma_wasm_storage_stack_bytes: () => 2097152,
      _postgamma_wasm_poll() {throw new Error('A failed instance must not enter native code for memory inspection');},
      _postgamma_wasm_allocator_live_bytes() {throw new Error('A failed instance must not take the allocator lock');},
    };
    globalThis.fixture = {module, tick: actualTick,
      async release() {released = true;},
      async exchange(_, request, signal) {
        if (signal?.aborted) throw new Error('Kernel instance failed');
        const op = new DataView(request).getUint32(8, true);
        const w = new Writer().u32(VERSION).u32(1).u32(op).u32(10);
        for (let i = 0; i < 17; i++) i === 2 ? w.text(op === Op.open ? 'OPEN failed' : 'STOP failed') : w.u32(0xffffffff);
        return w.finish();
      },
    };
    globalThis.self = {addEventListener() {}, close() {throw new Error('Control termination belongs to the parent transport');}};
    globalThis.postMessage = message => {
      assert(ownerStopped, 'The parent receives the fatal receipt after child shutdown gets an event-loop turn');
      messages.push(message); delivered(message);
    };
    await import(pathToFileURL(join(directory, 'worker/sql.js')) + '?failure=' + failure);
    const request = new Writer().u32(VERSION).u32(1).u32(Op.open)
      .u32(0).u32(4).text('/database').u32(1).text('db').text('').u32(0).finish();
    const broker = new MessageChannel();
    self.onmessage({data: {postgammaPthreadBroker: true}, ports: [broker.port2]});
    broker.port1.close(); broker.port2.close();
    self.onmessage({data: request});
    await Promise.race([cleaning, deadline]);
    assert.throws(() => globalThis.postgammaMemoryDiagnostic(), /healthy quiescent/);
    assert(!released && physicalHandle && messages.length === 0,
      'Ownership and fatal replies wait for in-flight I/O and physical close');
    assert(!nativeStoppedEarly, 'Native callers must remain live until the I/O owner stops entering native completion locks');
    assert.equal(nativeStops, 0);
    for (const worker of Object.values(module.PThread.pthreads)) worker.onmessage({data: {cmd: 'queued SDK cleanup'}});
    port.postMessage({quiesced: true});
    unblock();
    const result = await Promise.race([completed, deadline]);
    assert.throws(() => globalThis.postgammaMemoryDiagnostic(), /healthy quiescent/);
    assert(released && !physicalHandle);
    assert(!terminatedEarly && terminated === 0 && ownerStopped,
      'Released I/O owner closes itself without racing parent termination');
    assert(!nativeStoppedEarly, 'Native callers must remain live until the I/O owner stops entering native completion locks');
    assert.equal(nativeStops, 1);
    assert.equal(result.metrics.storageQuiesced, true);
    assert.equal(result.metrics.storageReleased, true);
    assert.equal(result.metrics.closedStorageHandles, 3);
    if (failure.startsWith('pthread')) {
      assert(pthreadErrorHandled, 'Owned pthread errors do not escape as unhandled browser errors');
      assert.match(result.fatal, /pthread failed/);
      assert.equal(result.metrics.crashedWorkerRetired, failure !== 'pthread-timeout');
      if (failure === 'pthread-driver') assert(driverRetired && !driverForced,
        'A failed pthread acknowledges its own JS event loop and closes without parent termination');
      if (failure === 'pthread-timeout') {
        assert(driverForced && !driverRetired);
        assert.match(result.fatal, /Failed pthread retirement timed out/);
      }
    } else assert.match(result.fatal, /OPEN failed.*STOP failed/);
    console.log(failure + ': I/O quiescence precedes native termination; physical close precedes ownership release and rejection');
    clearTimeout(timer); port.close();
  }
} finally {
  clearTimeout(timer); unblock?.(); port?.close();
  await rm(directory, {recursive: true, force: true});
}
