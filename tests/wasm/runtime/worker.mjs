/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
import createRuntime from './runtime.mjs';
const output = [];
try {
  const module = await createRuntime({print: text => output.push(text), printErr: text => { output.push(text); console.error(text); }});
  if (module._postgamma_runtime_test_start() !== 0) throw new Error('supervisor creation failed');
  let ticks = 0;
  const poll = () => {
    ticks++;
    const completed = module._postgamma_runtime_test_poll();
    if (!completed) { setTimeout(poll, 1); return; }
    postMessage({status: 'passed', completed, ticks, worker: true,
                 sharedMemory: module.HEAPU8.buffer instanceof SharedArrayBuffer, output});
  };
  poll();
} catch (error) {
  postMessage({status: 'failed', error: String(error.stack || error), output});
}
