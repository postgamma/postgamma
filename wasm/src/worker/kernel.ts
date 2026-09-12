/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
import {runKernelSmoke} from '../bindings/kernel.js';
import {loadKernel} from '../loader/kernel.js';

const output: string[] = [];
try {
  const module = await loadKernel({
    print: (text: string) => output.push(text),
    printErr: (text: string) => { output.push(text); console.error(text); },
  });
  const result = await runKernelSmoke(module);
  module.PThread.terminateAllThreads();
  if (module.PThread.unusedWorkers.length || Object.keys(module.PThread.pthreads).length)
    throw new Error('SDK Worker pool disposal was incomplete');
  postMessage({status: 'passed', ...result, worker: true,
    workerPoolDisposed: true,
    sharedMemory: module.wasmMemory.buffer instanceof SharedArrayBuffer,
    memoryBytes: module.wasmMemory.buffer.byteLength, output});
} catch (error) {
  postMessage({status: 'failed', error: error instanceof Error ? error.stack : String(error), output});
}
