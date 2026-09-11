/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
window.probeResult = null;
window.probeEvents = [];
const longjmp = new URL(location.href).searchParams.get('longjmp');
const report = {status: 'failed', isolated: crossOriginIsolated, worker: false,
  sharedMemory: false, longjmp, rounds: [], terminal: null};
const record = (event) => window.probeEvents.push({timeMs: performance.now(), ...event});
const delay = (ms) => new Promise((resolve) => setTimeout(resolve, ms));
const require = (condition, message) => { if (!condition) throw new Error(message); };
const WAITING = 2, RECOVERED = 3, DONE = 4;

function channel(name) {
  const worker = new Worker('./recovery-worker.mjs', {type: 'module'});
  const pending = new Map();
  let sequence = 0, failReady;
  const ready = new Promise((resolve, reject) => {
    failReady = reject;
    worker.onmessage = ({data}) => {
      record({source: name, data});
      if (data.kind === 'ready') resolve(data);
      else if (data.kind === 'fatal') {
        reject(new Error(data.error));
        for (const request of pending.values()) request.reject(new Error(data.error));
      } else if (pending.has(data.id)) {
        const request = pending.get(data.id);
        pending.delete(data.id);
        request.resolve(data);
      }
    };
    worker.onerror = (error) => {
      reject(new Error(error.message));
      for (const request of pending.values()) request.reject(new Error(error.message));
    };
  });
  const timeout = setTimeout(() => failReady(new Error(`${name}: startup timed out`)), 15000);
  async function call(command, extra = {}, expectTerminal = false) {
    const id = ++sequence;
    let timer;
    try {
      const response = await new Promise((resolve, reject) => {
        pending.set(id, {resolve, reject});
        timer = setTimeout(() => reject(new Error(`${name}/${command}: response timed out`)), 5000);
        worker.postMessage({id, command, ...extra});
      });
      if (expectTerminal) return response;
      if (response.error) throw new Error(response.error);
      require(!response.result?.error, JSON.stringify(response.result));
      return response.result;
    } finally {
      clearTimeout(timer);
      pending.delete(id);
    }
  }
  return {call, ready: ready.finally(() => clearTimeout(timeout)),
    close: () => { clearTimeout(timeout); worker.terminate(); }};
}

function validate(result) {
  require(result.failures === 0, `C recovery checks failed: ${JSON.stringify(result)}`);
  require(result.cases.length >= 15 && result.cases.every((c) => c.checks > 0), 'missing recovery cases');
  require(result.repetitions === 1000 && result.allocations === result.cleanups, 'recovery leaked resources');
  require(result.stack.minimumSP < result.stack.finalSP && result.stack.minimumSP >= result.stack.end,
    'deep jump did not restore the linear-memory stack');
  require(result.nativeExceptions === (longjmp === 'wasm'), 'compiled exception feature does not match the requested mode');
}

async function until(control, predicate) {
  const deadline = performance.now() + 5000;
  while (performance.now() < deadline) {
    const {stages} = await control.call('state');
    if (predicate(stages)) return stages;
    await delay(10);
  }
  throw new Error('recovery pthread state transition timed out');
}

const control = channel('recovery');
try {
  const ready = await control.ready;
  require(ready.isolated && ready.worker && ready.sharedMemory, 'isolated shared Worker required');
  report.worker = ready.worker;
  report.sharedMemory = ready.sharedMemory;
  report.control = await control.call('sync');
  validate(report.control);
  for (let round = 1; round <= 2; round++) {
    const result = {round, controlReplies: 0, foreign: [], threads: []};
    report.rounds.push(result);
    require((await control.call('start', {round})).rc === 0, 'recovery pthread startup failed');
    await until(control, (stages) => stages.every((stage) => stage === WAITING));
    for (let slot = 0; slot < 3; slot++) {
      const refusal = await control.call('foreign', {slot});
      result.foreign.push(refusal);
      require(refusal.rc === report.control.foreignErrno, 'foreign carrier was not rejected');
    }
    const heldAt = performance.now();
    do {
      const {stages} = await control.call('state');
      require(stages.every((stage) => stage === WAITING), 'thread left its live recovery boundary early');
      result.controlReplies++;
      await delay(20);
    } while (performance.now() - heldAt < 200);
    result.heldMs = performance.now() - heldAt;
    require((await control.call('release', {slot: 1})).rc === 0, 'error release failed');
    await until(control, (stages) => stages[1] === RECOVERED);
    result.isolatedStages = (await control.call('state')).stages;
    require(result.isolatedStages[0] === WAITING && result.isolatedStages[2] === WAITING,
      'one thread recovery affected its peers');
    for (const slot of [0, 2]) require((await control.call('release', {slot})).rc === 0, 'peer release failed');
    await until(control, (stages) => stages.every((stage) => stage === RECOVERED));
    for (let slot = 0; slot < 3; slot++) require((await control.call('release', {slot})).rc === 0, 'suite release failed');
    await until(control, (stages) => stages.every((stage) => stage === DONE));
    require((await control.call('join')).rc === 0, 'recovery pthread join failed');
    for (let slot = 0; slot < 3; slot++) {
      const thread = await control.call('report', {slot});
      result.threads.push(thread);
      validate(thread);
    }
    const all = [report.control, ...result.threads];
    require(new Set(all.map((r) => r.tlsAddress)).size === 4, 'exception roots alias across threads');
    for (let i = 0; i < all.length; i++) for (let j = 0; j < i; j++) {
      const a = all[i].stack, b = all[j].stack;
      require(a.base <= b.end || b.base <= a.end, 'recovery thread stacks overlap');
    }
    await delay(30);
  }

  const failed = channel('trap');
  try {
    await failed.ready;
    const trapped = await failed.call('trap', {}, true);
    report.terminal = trapped;
    require(trapped.terminal && trapped.errorName === 'RuntimeError' && /unreachable/.test(trapped.error),
      'Wasm trap did not remain a terminal runtime error');
    require(trapped.output.includes('recovery: trap armed inside PG_TRY') &&
      !trapped.output.includes('recovery: trap incorrectly caught as ERROR'), 'PG_TRY incorrectly caught a trap');
    report.afterTrap = await failed.call('sync');
    require(report.afterTrap.rejected && report.afterTrap.wasmCalled === false, 'failed instance was reused');
    report.independentInstance = await control.call('sync');
    validate(report.independentInstance);
  } finally {
    failed.close();
  }
  report.adaptations = [
    'The SDK sigsetjmp alias ignores savemask; restore the logical interrupt mask explicitly at the owning recovery boundary.',
    'Keep jump buffers and error callbacks on their owning live carrier; recreate them after thread or request reuse.',
    'Unwind and release resources in C before returning a recoverable error; a Wasm trap terminates the affected instance.',
  ];
  report.status = 'passed';
} catch (error) {
  report.error = String(error.stack || error);
} finally {
  control.close();
  window.probeResult = report;
  document.querySelector('#status').textContent = JSON.stringify(report);
}
