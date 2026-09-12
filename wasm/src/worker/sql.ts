/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
import {loadKernel} from '../loader/kernel.js';
import {exchange, tick} from '../bindings/sql.js';
import type {SqlModule} from '../bindings/sql.js';
import {LIMIT, Op, Reader, VERSION} from '../protocol/wire.js';
const output: string[] = [];
let queuedBytes = 0, peakQueuedBytes = 0;
let failed = false, pending = 0, peakPending = 0, peakMemory = 0, peakThreads = 0;
const kernel = loadKernel({print: text => output.push(text), printErr: text => {
  output.push(text); console.error(text);
}}).then(value => {
  const module = value as SqlModule;
  const status = module._postgamma_wasm_start();
  if (status) throw new Error(`Host driver failed: ${status}`);
  return module;
});
let queue = Promise.resolve();
function fail(error: unknown) {
  if (!failed) {failed = true; postMessage({fatal: String(error), output});}
}
kernel.catch(fail);
self.onmessage = ({data}) => {
  if (failed) return;
  if (!(data instanceof ArrayBuffer) || data.byteLength > LIMIT || ++pending > 64 || queuedBytes + data.byteLength > 8 * 1024 * 1024) {
    fail(new Error('Invalid or excessive Worker messages')); return;
  }
  queuedBytes += data.byteLength; peakQueuedBytes = Math.max(peakQueuedBytes, queuedBytes);
  peakPending = Math.max(peakPending, pending);
  queue = queue.then(async () => {
    const module = await kernel;
    const request = new Reader(data);
    if (request.u32() !== VERSION) throw new Error('Unsupported protocol');
    const sequence = request.u32(), op = request.u32();
    const reply = await exchange(module, data);
    peakMemory = Math.max(peakMemory, module.wasmMemory.buffer.byteLength);
    peakThreads = Math.max(peakThreads, Object.keys(module.PThread.pthreads).length);
    if (op === Op.stop) {
      const status = new Reader(reply); status.u32(); status.u32(); status.u32();
      if (status.u32() === 0) {
        while (module._postgamma_wasm_join() !== 0) await tick();
        while (Object.keys(module.PThread.pthreads).length) await tick();
        const returnedWorkers = module.PThread.unusedWorkers.length;
        module.PThread.terminateAllThreads();
        postMessage({sequence, reply, metrics: {peakMemory, peakThreads, peakPending, peakQueuedBytes, returnedWorkers,
          runningWorkers: Object.keys(module.PThread.pthreads).length, output}}, [reply]);
        return;
      }
    }
    postMessage({sequence, reply, sample: {memory: module.wasmMemory.buffer.byteLength,
      threads: Object.keys(module.PThread.pthreads).length, queuedBytes}}, [reply]);
  }).catch(fail).finally(() => {pending--; queuedBytes -= data.byteLength;});
};
