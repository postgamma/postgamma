/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
import createProbe from './recovery.mjs';

const output = [];
let terminal = null;
try {
  const module = await createProbe({
    print: (text) => output.push(text), printErr: (text) => output.push(text),
    onAbort: (reason) => {
      terminal = String(reason);
      postMessage({kind: 'fatal', error: terminal, output});
    },
  });
  const decode = (pointer) => JSON.parse(module.UTF8ToString(pointer));
  onmessage = ({data}) => {
    if (terminal) {
      postMessage({id: data.id, result: {rejected: true, reason: terminal, wasmCalled: false}});
      return;
    }
    try {
      let result;
      switch (data.command) {
        case 'sync': result = decode(module._probe_recovery_sync()); break;
        case 'start': result = {rc: module._probe_recovery_start(data.round)}; break;
        case 'state': result = {stages: [0, 1, 2].map((i) => module._probe_recovery_state(i))}; break;
        case 'foreign': result = {rc: module._probe_recovery_foreign(data.slot)}; break;
        case 'release': result = {rc: module._probe_recovery_release(data.slot)}; break;
        case 'report': result = decode(module._probe_recovery_report(data.slot)); break;
        case 'join': result = {rc: module._probe_recovery_join()}; break;
        case 'trap': module._probe_recovery_trap(); result = {returned: true}; break;
        default: throw new Error(`Unknown recovery probe command: ${data.command}`);
      }
      postMessage({id: data.id, result, output: [...output]});
    } catch (error) {
      terminal = String(error.stack || error);
      postMessage({id: data.id, terminal: true, error: terminal,
        errorName: error.name, output: [...output]});
    }
  };
  postMessage({kind: 'ready', isolated: crossOriginIsolated,
    worker: typeof document === 'undefined', sharedMemory: module.HEAPU8.buffer instanceof SharedArrayBuffer});
} catch (error) {
  postMessage({kind: 'fatal', error: String(error.stack || error), output});
}
