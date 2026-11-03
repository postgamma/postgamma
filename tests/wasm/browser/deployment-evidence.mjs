/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
export const scenarios = [
  {name: 'isolated', expected: null},
  {name: 'no-coop', remove: ['cross-origin-opener-policy'], expected: 'ISOLATION_REQUIRED'},
  {name: 'no-coep', remove: ['cross-origin-embedder-policy'], expected: 'ISOLATION_REQUIRED'},
  {name: 'no-isolation', remove: ['cross-origin-opener-policy', 'cross-origin-embedder-policy'], expected: 'ISOLATION_REQUIRED'},
  {name: 'no-worker', capability: 'Worker', expected: 'WORKER_REQUIRED'},
  {name: 'no-shared-memory', capability: 'SharedArrayBuffer', expected: 'SHARED_MEMORY_REQUIRED'},
  {name: 'no-wasm', capability: 'WebAssembly', expected: 'WASM_REQUIRED'},
  {name: 'missing-worker', asset: 'deployment-worker.mjs', expected: 'WORKER_LOAD_FAILED'},
  {name: 'missing-module', asset: 'deployment.mjs', expected: 'MODULE_LOAD_FAILED'},
  {name: 'missing-wasm', asset: 'deployment.wasm', expected: 'WASM_LOAD_FAILED'},
  {name: 'corrupt-wasm', asset: 'deployment.wasm', corrupt: true, expected: 'WASM_COMPILE_FAILED'},
];

export function assess(scenario, result, requests, pageErrors) {
  if (!result || pageErrors.length) return 'failed';
  const allowed = scenario.asset && !scenario.corrupt ? scenario.asset : null;
  if (requests.some((r) => !allowed || new URL(r.url).pathname.split('/').at(-1) !== allowed ||
      (r.status !== undefined && r.status !== 404))) return 'failed';
  if (scenario.expected)
    return result.status === 'rejected' && result.code === scenario.expected &&
      result.probeStarted === false ? 'passed' : 'failed';
  if (result.status === 'rejected') {
    const missing = {ISOLATION_REQUIRED: result.capabilities?.isolated === false,
      WORKER_REQUIRED: result.capabilities?.worker === false,
      SHARED_MEMORY_REQUIRED: result.capabilities?.sharedArrayBuffer === false,
      WASM_REQUIRED: result.capabilities?.wasm === false};
    return missing[result.code] === true ? 'unsupported' : 'failed';
  }
  return result.status === 'passed' && result.isolated && result.worker && result.sharedMemory &&
    result.rounds?.length === 2 && result.rounds.every((r) => r.controlReplies >= 3 &&
      r.threads?.length === 3 && r.threads.every((t) => t.done && t.value === t.expected) &&
      new Set(r.threads.map((t) => t.tlsAddress)).size === 3) ? 'passed' : 'failed';
}
