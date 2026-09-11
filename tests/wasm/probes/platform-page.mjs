/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
window.probeResult = null;
window.probeEvents = [];
const report = {status: 'failed', isolated: crossOriginIsolated, worker: false,
  sharedMemory: false, rounds: [], overflow: []};
const record = (event) => window.probeEvents.push({timeMs: performance.now(), ...event});
const delay = (ms) => new Promise((resolve) => setTimeout(resolve, ms));
const require = (condition, message) => { if (!condition) throw new Error(message); };

async function positive() {
  const worker = new Worker('./platform-worker.mjs', {type: 'module'});
  const pending = new Map();
  let sequence = 0, failReady;
  const ready = new Promise((resolve, reject) => {
    failReady = reject;
    worker.onmessage = ({data}) => {
      record({source: 'platform', data});
      if (data.kind === 'ready') resolve(data);
      else if (data.kind === 'fatal') {
        reject(new Error(data.error));
        for (const request of pending.values()) request.reject(new Error(data.error));
      } else if (pending.has(data.id)) {
        const request = pending.get(data.id);
        pending.delete(data.id);
        data.error ? request.reject(new Error(data.error)) : request.resolve(data.result);
      }
    };
    worker.onerror = (error) => {
      reject(new Error(error.message));
      for (const request of pending.values()) request.reject(new Error(error.message));
    };
  });
  const readyTimeout = setTimeout(() => failReady(new Error('platform Worker startup timed out')), 15000);
  async function call(command, extra = {}) {
    const id = ++sequence;
    let timer;
    try {
      return await new Promise((resolve, reject) => {
        pending.set(id, {resolve, reject});
        timer = setTimeout(() => reject(new Error(`${command}: control Worker response timed out`)), 5000);
        worker.postMessage({id, command, ...extra});
      });
    } finally {
      clearTimeout(timer);
      pending.delete(id);
    }
  }
  async function stateUntil(predicate) {
    const deadline = performance.now() + 5000;
    while (performance.now() < deadline) {
      const state = await call('state');
      if (predicate(state)) return state;
      await delay(10);
    }
    throw new Error('pthread state transition timed out');
  }
  try {
    const started = await ready;
    clearTimeout(readyTimeout);
    require(started.isolated && started.worker && started.sharedMemory, 'isolated shared Worker required');
    report.worker = started.worker;
    report.sharedMemory = started.sharedMemory;
    report.baseline = await call('sync');
    require(report.baseline.failures === 0, 'ABI/libc checks failed');
    require(report.baseline.bigInt.passed, '64-bit JS/C ABI roundtrip failed');
    require(report.baseline.entropy.providerCalls >= 3 && report.baseline.entropy.providerBytes >= 96,
      'entropy did not reach the browser Web Crypto provider');
    for (let round = 1; round <= 2; round++) {
      require((await call('start', {round})).rc === 0, 'pthread startup failed');
      await stateUntil((state) => state.ready === 3 && state.done === 0);
      const heldAt = performance.now(), latencyMs = [];
      // All three threads remain live; these messages must execute in their owning Worker.
      do {
        const sentAt = performance.now();
        const state = await call('state');
        latencyMs.push(performance.now() - sentAt);
        require(state.ready === 3 && state.done === 0, 'a thread exited before explicit release');
        await delay(30);
      } while (performance.now() - heldAt < 300 || latencyMs.length < 10);
      const heldMs = performance.now() - heldAt;
      await call('release');
      await stateUntil((state) => state.done === 3);
      const result = await call('finish');
      result.control = {heldMs, responses: latencyMs.length, latencyMs};
      report.rounds.push(result);
      require(result.failures === 0, `pthread round ${round} failed`);
      // Let SDK cleanup messages return joined Workers to the precreated pool.
      await delay(30);
    }
  } finally {
    clearTimeout(readyTimeout);
    worker.terminate();
  }
}

async function overflow(mode) {
  const worker = new Worker(`./stack-overflow-worker.mjs?mode=${mode}`, {type: 'module'});
  const result = {mode, status: 'failed', events: []};
  report.overflow.push(result);
  let timer, settle;
  try {
    await new Promise((resolve, reject) => {
      timer = setTimeout(() => reject(new Error(`${mode}: no stack overflow diagnostic received`)), 15000);
      worker.onerror = (error) => reject(new Error(error.message));
      worker.onmessage = ({data}) => {
        record({source: `overflow-${mode}`, data});
        result.events.push(data);
        if (data.kind === 'fatal') reject(new Error(data.error));
        if (data.kind === 'abort') {
          if (!data.armed || !/stack overflow/i.test(data.reason))
            reject(new Error(`unexpected abort: ${data.reason}`));
          else {
            result.diagnostic = data.reason;
            const bounds = data.output.join('\n').match(/stack-overflow: armed base=(\d+) end=(\d+)/);
            if (!bounds || Number(bounds[1]) - Number(bounds[2]) !== 65536) {
              reject(new Error('overflow probe did not report a 64 KiB stack'));
              return;
            }
            result.bounds = {base: Number(bounds[1]), end: Number(bounds[2]), size: 65536};
            // Preserve the forwarded pthread exception too; unrelated errors still fail.
            settle = setTimeout(resolve, 100);
          }
        }
      };
    });
    require(result.events.some((e) => e.kind === 'armed' && e.sharedMemory), 'overflow instance was not armed');
    result.status = 'passed';
  } finally {
    clearTimeout(timer);
    clearTimeout(settle);
    worker.terminate();
  }
}

try {
  require(crossOriginIsolated && typeof SharedArrayBuffer !== 'undefined', 'cross-origin isolation required');
  await positive();
  await overflow('control');
  await overflow('pthread');
  report.adaptations = [
    'Normalize unsigned i64 exports with BigInt.asUintN(64, value); the raw Wasm export returns a signed BigInt.',
    'Use process-private semaphores for threads; the SDK rejects pshared=1 with a direct ENOTSUP return.',
    'Use a deliberate PostgreSQL locale provider; accepted locale names still use bytewise strcoll.',
    'Use measured linear-memory stack bounds and explicit overflow checks; guard attributes do not prove protected pages.',
  ];
  report.status = 'passed';
} catch (error) {
  report.error = String(error.stack || error);
} finally {
  window.probeResult = report;
  document.querySelector('#status').textContent = JSON.stringify(report);
}
