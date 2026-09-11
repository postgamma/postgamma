/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
import createProbe from './opfs.mjs';

const output = [];
try {
  const module = await createProbe({
    print: (text) => output.push(text), printErr: (text) => output.push(text),
    onAbort: (reason) => postMessage({kind: 'fatal', error: String(reason), output}),
  });
  const decode = (pointer) => JSON.parse(module.UTF8ToString(pointer));
  onmessage = ({data}) => {
    try {
      let result;
      switch (data.command) {
        case 'start': result = {rc: module._opfs_start(data.verify || 0)}; break;
        case 'state': result = decode(module._opfs_state()); break;
        case 'release': module._opfs_release(); result = {released: true}; break;
        case 'report': result = decode(module._opfs_report()); break;
        case 'metrics': result = decode(module._opfs_metrics(data.group)); break;
        default: throw new Error(`Unknown OPFS command: ${data.command}`);
      }
      postMessage({id: data.id, result});
    } catch (error) {
      postMessage({id: data.id, error: String(error.stack || error), output});
    }
  };
  postMessage({kind: 'ready', isolated: crossOriginIsolated,
    worker: typeof document === 'undefined', sharedMemory: module.HEAPU8.buffer instanceof SharedArrayBuffer,
    opfs: typeof navigator.storage?.getDirectory === 'function' && typeof FileSystemSyncAccessHandle !== 'undefined'});
} catch (error) {
  postMessage({kind: 'fatal', error: String(error.stack || error), output});
}
