/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
window.probeResult = null;
const capabilities = {isolated: crossOriginIsolated, worker: typeof Worker === 'function',
  sharedArrayBuffer: typeof SharedArrayBuffer === 'function', wasm: typeof WebAssembly === 'object'};
const report = {status: 'failed', capabilities, isolated: crossOriginIsolated,
  kernelStarted: false, probeStarted: false, rounds: []};
const delay = (ms) => new Promise((resolve) => setTimeout(resolve, ms));
const require = (condition, message) => { if (!condition) throw new Error(message); };
let worker;

function reject(code, error) {
  Object.assign(report, {status: 'rejected', code, error});
  throw Object.assign(new Error(error), {rejected: true});
}

try {
  if (!capabilities.worker) reject('WORKER_REQUIRED', 'The threaded probe requires a Worker.');
  if (!capabilities.wasm) reject('WASM_REQUIRED', 'WebAssembly is required.');
  if (!capabilities.isolated) reject('ISOLATION_REQUIRED', 'Serve the page with COOP same-origin and COEP require-corp.');
  if (!capabilities.sharedArrayBuffer) reject('SHARED_MEMORY_REQUIRED', 'SharedArrayBuffer is required.');
  worker = new Worker('./deployment-worker.mjs', {type: 'module'});
  const pending = new Map();
  let sequence = 0, timer;
  const ready = await new Promise((resolve, fail) => {
    timer = setTimeout(() => fail(new Error('Deployment worker startup timed out')), 8000);
    worker.onmessage = ({data}) => {
      if (data.kind === 'ready') resolve(data);
      else if (data.kind === 'rejected') resolve(data);
      else if (pending.has(data.id)) {
        const request = pending.get(data.id);
        if (data.error) request.reject(new Error(data.error)); else request.resolve(data.result);
      }
    };
    worker.onerror = (event) => {
      event.preventDefault();
      resolve({kind: 'rejected', code: 'WORKER_LOAD_FAILED', error: event.message || 'Worker module could not load.'});
      for (const request of pending.values()) request.reject(new Error(event.message || 'Worker failed.'));
    };
  }).finally(() => clearTimeout(timer));
  if (ready.kind === 'rejected') reject(ready.code, ready.error);
  report.worker = ready.worker;
  report.sharedMemory = ready.sharedMemory;
  report.heapBytes = ready.heapBytes;
  report.output = ready.output;
  require(ready.worker && ready.isolated && ready.sharedMemory, 'Worker shared-memory initialization failed');

  async function call(command, extra = {}) {
    let timeout;
    const id = ++sequence;
    try {
      return await new Promise((resolve, reject) => {
        pending.set(id, {resolve, reject});
        timeout = setTimeout(() => reject(new Error(`${command}: control response timed out`)), 4000);
        worker.postMessage({id, command, ...extra});
      });
    } finally { clearTimeout(timeout); pending.delete(id); }
  }
  async function until(expected) {
    const deadline = performance.now() + 4000;
    while (performance.now() < deadline) {
      if ((await call('state')).states.every((value) => value === expected)) return;
      await delay(5);
    }
    throw new Error('C pthread state transition timed out');
  }
  for (let round = 1; round <= 2; round++) {
    report.probeStarted = true;
    require((await call('start', {round})).rc === 0, 'C pthread startup failed');
    await until(1);
    const item = {round, controlReplies: 0};
    const start = performance.now();
    do {
      require((await call('state')).states.every((state) => state === 1), 'C callers returned before release');
      item.controlReplies++;
      await delay(10);
    } while (performance.now() - start < 100);
    item.heldMs = performance.now() - start;
    await call('release');
    await until(2);
    Object.assign(item, await call('join'));
    report.rounds.push(item);
  }
  report.opfs = await call('opfs');
  report.status = 'passed';
} catch (error) {
  if (!error.rejected) report.error = String(error.stack || error);
} finally {
  if (worker) worker.terminate();
  window.probeResult = report;
  document.querySelector('#status').textContent = JSON.stringify(report);
}
