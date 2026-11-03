/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
import createProbe from './smoke.mjs';

try {
  const output = [];
  const module = await createProbe({
    print: (text) => output.push(text),
    printErr: (text) => output.push(text),
  });
  const results = [module._postgamma_probe(21), module._postgamma_probe(7)];
  postMessage({
    status: results[0] === 42 && results[1] === 14 ? 'passed' : 'failed',
    results,
    sharedMemory: module.HEAPU8.buffer instanceof SharedArrayBuffer,
    worker: typeof document === 'undefined',
    output,
  });
} catch (error) {
  postMessage({status: 'failed', error: String(error.stack || error)});
}
