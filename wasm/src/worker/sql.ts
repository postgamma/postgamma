/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
import {loadKernel} from '../loader/kernel.js';
import {exchange, tick} from '../bindings/sql.js';
import type {SqlModule} from '../bindings/sql.js';
import {DatabaseError, LIMIT, Op, Reader, VERSION, Writer, response} from '../protocol/wire.js';
import {limits} from '../protocol/limits.js';
import {acquire} from '../storage/ownership.js';
const output: string[] = [];
let droppedLogs = 0;
function record(text: string) {
  if (output.length === limits.logLines) {output.shift(); droppedLogs++;}
  output.push(text.slice(0, limits.logLineCharacters));
}
let loaded: SqlModule | undefined;
let ownership: (() => Promise<void>) | undefined;
const stopped = new AbortController();
let queuedBytes = 0, peakQueuedBytes = 0;
let failed = false, pending = 0, peakPending = 0, peakMemory = 0, peakThreads = 0;
let samples = 0, periodicSamples = 0, samplingIntervalMs = 0;
let ioStackBytes = 0;
function sample() {
  if (!loaded) return;
  samples++;
  peakMemory = Math.max(peakMemory, loaded.wasmMemory.buffer.byteLength);
  peakThreads = Math.max(peakThreads, Object.keys(loaded.PThread.pthreads).length);
}
let sampler: ReturnType<typeof setInterval> | undefined;
async function fail(error: unknown) {
  if (failed) return;
  sample(); clearInterval(sampler);
  failed = true; stopped.abort();
  const interruptedWorkers = loaded ? Object.keys(loaded.PThread.pthreads).length : null;
  loaded?.PThread.terminateAllThreads();
  const release = ownership; ownership = undefined;
  let failure = String(error);
  try {await release?.();}
  catch (cause) {failure += '; ownership release failed: ' + String(cause);}
  postMessage({fatal: failure, output, metrics: {peakMemory, peakThreads, peakPending,
    peakQueuedBytes, samples, droppedLogs, interruptedWorkers, terminationRequested: true,
    ioStackBytes,
    runningWorkers: loaded ? Object.keys(loaded.PThread.pthreads).length : null}});
  self.close();
}
self.addEventListener('error', event => {event.preventDefault(); fail(event.message);});
self.addEventListener('unhandledrejection', event => {event.preventDefault(); fail(event.reason);});
let kernel: Promise<SqlModule> | undefined;
function startKernel() {return loadKernel({print: record, printErr: text => {
  record(text); console.error(text.slice(0, limits.logLineCharacters));
}, onAbort: fail}).then(value => {
  const module = value as SqlModule;
  loaded = module;
  module.postgammaBridgeEvent = (kind, id, generation) => {
    if (kind === 0) module.postgammaBridgeReply?.();
    else postMessage({ready: {kind, id, generation}});
  };
  sample();
  if (samplingIntervalMs) sampler = setInterval(() => {periodicSamples++; sample();}, samplingIntervalMs);
  const status = module._postgamma_wasm_start();
  if (status) throw new Error(`Host driver failed: ${status}`);
  return module;
});}
async function dispose(module: SqlModule) {
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
  await ownership?.(); ownership = undefined;
  return {peakMemory, peakThreads, peakPending, peakQueuedBytes, samples, periodicSamples, samplingIntervalMs,
    ioStackBytes,
    driverWakeCount: module._postgamma_wasm_wake_count(), droppedLogs, returnedWorkers,
    runningWorkers: Object.keys(module.PThread.pthreads).length, output};
}
let queue = Promise.resolve();
self.onmessage = ({data}) => {
  if (failed) return;
  if (!(data instanceof ArrayBuffer) || data.byteLength > LIMIT || ++pending > limits.maxPendingCommands || queuedBytes + data.byteLength > limits.workerQueueBytes) {
    fail(new Error('Invalid or excessive Worker messages')); return;
  }
  queuedBytes += data.byteLength; peakQueuedBytes = Math.max(peakQueuedBytes, queuedBytes);
  peakPending = Math.max(peakPending, pending);
  queue = queue.then(async () => {
    if (failed) return;
    const request = new Reader(data);
    if (request.u32() !== VERSION) throw new Error('Unsupported protocol');
    const sequence = request.u32(), op = request.u32();
    if (!kernel) {
      if (op !== Op.open) throw new Error('Instance must be opened first');
      request.u32(); request.u32(); request.text();
      const storage = request.u32(), name = request.text(); request.text();
      samplingIntervalMs = request.u32(); request.end();
      if (samplingIntervalMs > 60000) throw new Error('Invalid diagnostic sampling interval');
      if (storage === 1) ownership = await acquire(name!);
      kernel = startKernel();
    }
    const module = await kernel;
    const reply = await exchange(module, data, stopped.signal);
    if (op === Op.open) ioStackBytes = module._postgamma_wasm_storage_stack_bytes();
    sample();
    if (op === Op.open && new DataView(reply).getUint32(12, true) !== 0) {
      const stoppedReply = await exchange(module, new Writer().u32(VERSION).u32(sequence).u32(Op.stop).finish());
      try {response(stoppedReply, sequence, Op.stop).end();}
      catch (teardownError) {
        let openingError: unknown;
        try {response(reply, sequence, Op.open);}
        catch (cause) {openingError = cause;}
        const detail = openingError instanceof DatabaseError ? openingError.diagnostics[3] : null;
        throw new Error([String(openingError), detail, 'Startup teardown failed: ' + String(teardownError)]
          .filter(Boolean).join('; '));
      }
      postMessage({sequence, reply, metrics: await dispose(module)}, [reply]); return;
    }
    if (op === Op.stop) {
      const status = new Reader(reply); status.u32(); status.u32(); status.u32();
      if (status.u32() === 0) {
        postMessage({sequence, reply, metrics: await dispose(module)}, [reply]);
        return;
      }
      throw new Error('Storage teardown failed; the instance has been discarded');
    }
    postMessage({sequence, reply, sample: {memory: module.wasmMemory.buffer.byteLength,
      threads: Object.keys(module.PThread.pthreads).length, queuedBytes, periodicSamples,
      driverWakeCount: module._postgamma_wasm_wake_count()}}, [reply]);
  }).catch(fail).finally(() => {pending--; queuedBytes -= data.byteLength;});
};
