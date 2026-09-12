/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
import createProbe from './resource.mjs';
try {
  const output = [];
  const module = await createProbe({print: (s) => output.push(s), printErr: (s) => output.push(s)});
  const decode = (p) => JSON.parse(module.UTF8ToString(p));
  onmessage = ({data: {id, command}}) => {
    try {
      let result;
      switch (command) {
        case 'snapshot': result = decode(module._resource_snapshot()); break;
        case 'start': result = {rc: module._resource_start()}; break;
        case 'grow': {
          const old = module.HEAPU8;
          result = decode(module._resource_grow());
          result.oldViewBytes = old.byteLength;
          result.refreshedViewBytes = module.HEAPU8.byteLength;
          result.newRegionReadable = module.HEAPU8[result.pointer + result.requestedBytes - 4096] ===
            ((result.requestedBytes - 4096) / 4096 & 255);
          break;
        }
        case 'release': module._resource_release(); result = {}; break;
        case 'join': result = decode(module._resource_join()); break;
        default: throw new Error(`Unknown resource command: ${command}`);
      }
      postMessage({id, result});
    } catch (error) { postMessage({id, error: String(error.stack || error)}); }
  };
  postMessage({kind: 'ready', worker: typeof document === 'undefined', isolated: crossOriginIsolated,
    sharedMemory: module.HEAPU8.buffer instanceof SharedArrayBuffer, output});
} catch (error) { postMessage({kind: 'fatal', error: String(error.stack || error)}); }
