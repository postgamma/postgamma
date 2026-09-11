/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
import createProbe from './wait.mjs';

const output = [];
try {
  const module = await createProbe({
    print: (text) => output.push(text), printErr: (text) => output.push(text),
    onAbort: (reason) => postMessage({kind: 'fatal', error: String(reason), output}),
  });
  const decode = (pointer) => JSON.parse(module.UTF8ToString(pointer));
  const snapshot = (slot, full = 0) => decode(module._probe_wait_snapshot(slot, full));
  const apply = (data) => module._probe_wait_action(data.slot, data.request, data.action, data.timer || 0, data.value || 0);
  const rc = module._probe_wait_init();
  if (rc !== 0) throw new Error(`wait probe init failed: ${rc}`);
  onmessage = async ({data}) => {
    try {
      let result;
      switch (data.command) {
        case 'clocks': {
          const beforeMs = performance.timeOrigin + performance.now();
          result = decode(module._probe_wait_clocks());
          result.afterMs = performance.timeOrigin + performance.now();
          result.beforeMs = beforeMs;
          break;
        }
        case 'start':
          result = {rc: module._probe_wait_start(data.slot, data.request, data.ownMs, data.timerMs, data.gate),
            snapshot: snapshot(data.slot)};
          break;
        case 'snapshot': result = snapshot(data.slot, data.full || 0); break;
        case 'action': result = {rc: apply(data), snapshot: snapshot(data.slot)}; break;
        case 'gate':
          result = {rc: module._probe_wait_release_gate(data.slot, data.request)};
          break;
        case 'join': result = {rc: module._probe_wait_join(data.slot, data.request)}; break;
        case 'race': {
          const results = [];
          for (const [index, action] of data.actions.entries()) {
            if (index && data.gapMs) await new Promise((resolve) => setTimeout(resolve, data.gapMs));
            results.push({action, rc: apply({...data, action})});
          }
          result = {actions: results, snapshot: snapshot(data.slot)};
          break;
        }
        case 'block-control': {
          // The backend's virtual deadline must fire while this Worker's JS
          // event loop cannot service timers or proxy requests.
          const rc = apply({...data, action: 4, value: 150});
          const before = decode(module._probe_wait_clocks());
          const blockResult = Atomics.wait(new Int32Array(new SharedArrayBuffer(4)), 0, 0, 400);
          const after = decode(module._probe_wait_clocks());
          result = {rc, before, after, blockResult, snapshot: snapshot(data.slot)};
          break;
        }
        default: throw new Error(`Unknown wait probe command: ${data.command}`);
      }
      postMessage({id: data.id, result});
    } catch (error) {
      postMessage({id: data.id, error: String(error.stack || error), output});
    }
  };
  postMessage({kind: 'ready', isolated: crossOriginIsolated,
    worker: typeof document === 'undefined', sharedMemory: module.HEAPU8.buffer instanceof SharedArrayBuffer});
} catch (error) {
  postMessage({kind: 'fatal', error: String(error.stack || error), output});
}
