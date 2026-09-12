/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
import {loadKernel} from '../loader/kernel.js';
import {exchange, tick} from '../bindings/sql.js';
import type {SqlModule} from '../bindings/sql.js';
import {LIMIT, Op, Reader, VERSION} from '../protocol/wire.js';
import {limits} from '../protocol/limits.js';
const output: string[] = [];
let droppedLogs = 0;
function record(text: string) {
  if (output.length === limits.logLines) {output.shift(); droppedLogs++;}
  output.push(text.slice(0, limits.logLineCharacters));
}
let loaded: SqlModule | undefined;
const stopped = new AbortController();
let queuedBytes = 0, peakQueuedBytes = 0;
let failed = false, pending = 0, peakPending = 0, peakMemory = 0, peakThreads = 0;
let samples = 0;
function sample() {
  if (!loaded) return;
  samples++;
  peakMemory = Math.max(peakMemory, loaded.wasmMemory.buffer.byteLength);
  peakThreads = Math.max(peakThreads, Object.keys(loaded.PThread.pthreads).length);
}
const sampler = setInterval(sample, 1);
function fail(error: unknown) {
  if (failed) return;
  sample(); clearInterval(sampler);
  failed = true; stopped.abort();
  const interruptedWorkers = loaded ? Object.keys(loaded.PThread.pthreads).length : null;
  loaded?.PThread.terminateAllThreads();
  postMessage({fatal: String(error), output, metrics: {peakMemory, peakThreads, peakPending,
    peakQueuedBytes, samples, droppedLogs, interruptedWorkers, terminationRequested: true,
    runningWorkers: loaded ? Object.keys(loaded.PThread.pthreads).length : null}});
  self.close();
}
self.addEventListener('error', event => {event.preventDefault(); fail(event.message);});
self.addEventListener('unhandledrejection', event => {event.preventDefault(); fail(event.reason);});
const kernel = loadKernel({print: record, printErr: text => {
  record(text); console.error(text.slice(0, limits.logLineCharacters));
}, onAbort: fail}).then(value => {
  const module = value as SqlModule;
  loaded = module;
  const status = module._postgamma_wasm_start();
  if (status) throw new Error(`Host driver failed: ${status}`);
  return module;
});
let queue = Promise.resolve();
kernel.catch(fail);
self.onmessage = ({data}) => {
  if (failed) return;
  if (!(data instanceof ArrayBuffer) || data.byteLength > LIMIT || ++pending > limits.maxPendingCommands || queuedBytes + data.byteLength > limits.workerQueueBytes) {
    fail(new Error('Invalid or excessive Worker messages')); return;
  }
  queuedBytes += data.byteLength; peakQueuedBytes = Math.max(peakQueuedBytes, queuedBytes);
  peakPending = Math.max(peakPending, pending);
  queue = queue.then(async () => {
    const module = await kernel;
    if (failed) return;
    const request = new Reader(data);
    if (request.u32() !== VERSION) throw new Error('Unsupported protocol');
    const sequence = request.u32(), op = request.u32();
    const reply = await exchange(module, data, stopped.signal);
    sample();
    if (op === Op.stop) {
      const status = new Reader(reply); status.u32(); status.u32(); status.u32();
      if (status.u32() === 0) {
        for (;;) {
          const joined = module._postgamma_wasm_join();
          if (joined === 0) break;
          if (joined !== 10) throw new Error(`Host join failed: ${joined}`);
          await tick();
        }
        while (Object.keys(module.PThread.pthreads).length) await tick();
        const returnedWorkers = module.PThread.unusedWorkers.length;
        sample(); clearInterval(sampler);
        module.PThread.terminateAllThreads();
        postMessage({sequence, reply, metrics: {peakMemory, peakThreads, peakPending, peakQueuedBytes, samples, droppedLogs, returnedWorkers,
          runningWorkers: Object.keys(module.PThread.pthreads).length, output}}, [reply]);
        return;
      }
    }
    postMessage({sequence, reply, sample: {memory: module.wasmMemory.buffer.byteLength,
      threads: Object.keys(module.PThread.pthreads).length, queuedBytes}}, [reply]);
  }).catch(fail).finally(() => {pending--; queuedBytes -= data.byteLength;});
};
