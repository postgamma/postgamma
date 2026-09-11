/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
window.probeResult = null;
window.probeEvents = [];
const report = {status: 'failed', isolated: crossOriginIsolated, worker: false, sharedMemory: false,
  kernelValidated: false, posixStorageComplete: false, inflightClose: [], metrics: []};
const require = (condition, message) => { if (!condition) throw new Error(message); };
const delay = (ms) => new Promise((resolve) => setTimeout(resolve, ms));
const record = (event) => window.probeEvents.push({timeMs: performance.now(), ...event});

function channel() {
  const worker = new Worker('./opfs-worker.mjs', {type: 'module'});
  const pending = new Map();
  let sequence = 0, readyTimer;
  const ready = new Promise((resolve, reject) => {
    readyTimer = setTimeout(() => reject(new Error('OPFS worker startup timed out')), 15000);
    const fail = (error) => {
      reject(error);
      for (const request of pending.values()) request.reject(error);
    };
    worker.onmessage = ({data}) => {
      if (data.kind === 'ready') resolve(data);
      else if (data.kind === 'fatal') { record(data); fail(new Error(data.error)); }
      else if (pending.has(data.id)) {
        const request = pending.get(data.id);
        if (data.error) { record(data); request.reject(new Error(data.error)); }
        else request.resolve(data.result);
      }
    };
    worker.onerror = (error) => fail(new Error(error.message));
  });
  async function call(command, extra = {}) {
    const id = ++sequence;
    let timer;
    try {
      return await new Promise((resolve, reject) => {
        pending.set(id, {resolve, reject});
        timer = setTimeout(() => reject(new Error(`${command}: OPFS control response timed out`)), 5000);
        worker.postMessage({id, command, ...extra});
      });
    } finally { clearTimeout(timer); pending.delete(id); }
  }
  return {call, ready: ready.finally(() => clearTimeout(readyTimer)), close: () => worker.terminate()};
}

async function completion(control, verify = false) {
  const deadline = performance.now() + 20000;
  let lastStage = -1, lastGate = 0;
  if (!verify) report.asyncControlReplies = 0;
  while (performance.now() < deadline) {
    const state = await control.call('state');
    if (state.stage !== lastStage) { record({verify, ...state}); lastStage = state.stage; }
    if (state.stage === 7) return;
    if (state.directoryPending) report.asyncControlReplies++;
    if (state.stage === 3 && state.gateEntered && state.closeStarted && state.gateRound > lastGate) {
      const held = performance.now();
      let replies = 0;
      do {
        const blocked = await control.call('state');
        require(blocked.gateEntered && blocked.closeStarted && !blocked.closeDone,
          'close released the shared access handle before inflight I/O completed');
        replies++;
        await delay(10);
      } while (performance.now() - held < 150);
      report.inflightClose.push({round: state.gateRound, peerDescriptorOpen: state.gateRound === 1,
        heldMs: performance.now() - held, controlReplies: replies, closeWaited: true});
      lastGate = state.gateRound;
      await control.call('release');
    }
    await delay(5);
  }
  throw new Error('OPFS C caller/owner progress timed out');
}

function summarize(metric) {
  const names = ['empty-proxy', 'read-8KiB', 'write-8KiB', 'flush', 'copy-8KiB-calibration'];
  const fields = ['totalMs', 'beforeOwnerMs', 'ownerApiMs', 'afterOwnerMs'];
  metric.operation = names[metric.operation];
  require(metric.samples.length === 64 * metric.callers, 'missing benchmark samples');
  metric.columns = ['caller', 'startEpochMs', ...fields];
  metric.percentiles = {};
  for (let i = 0; i < fields.length; i++) {
    const values = metric.samples.map((s) => s[i + 2]).sort((a, b) => a - b);
    // Different Worker clocks are coarsened independently. Preserve signed
    // boundary deltas; only same-realm elapsed intervals must be nonnegative.
    require(values.every((n) => Number.isFinite(n) && ((i === 1 || i === 3) || n >= 0)), 'invalid benchmark time');
    metric.percentiles[fields[i]] = {p50: values[Math.ceil(values.length * 0.5) - 1],
      p95: values[Math.ceil(values.length * 0.95) - 1], minimum: values[0],
      negativeSamples: values.filter((n) => n < 0).length};
  }
  const elapsedMs = Math.max(...metric.samples.map((s) => s[1] + s[2])) -
                    Math.min(...metric.samples.map((s) => s[1]));
  metric.throughputCallsPerSecond = metric.samples.length * 1000 / elapsedMs;
  return metric;
}

let control = channel();
try {
  const ready = await control.ready;
  require(ready.isolated && ready.worker && ready.sharedMemory && ready.opfs, 'isolated OPFS worker required');
  report.worker = ready.worker;
  report.sharedMemory = ready.sharedMemory;
  require((await control.call('start')).rc === 0, 'OPFS C caller startup failed');
  await completion(control);
  report.semantics = await control.call('report');
  for (let group = 0; group < 10; group++) report.metrics.push(summarize(await control.call('metrics', {group})));
  require(report.semantics.failures === 0, `OPFS checks failed: ${JSON.stringify(report.semantics.cases.filter((c) => c.failed))}`);
  require(report.inflightClose.length === 2 && report.asyncControlReplies > 0, 'missing concurrent progress evidence');
  report.adaptersRequired = report.semantics.cases.filter((c) => c.capability === 'adapter-required').map((c) => c.name);
  control.close();
  await delay(50);
  control = channel();
  await control.ready;
  require((await control.call('start', {verify: 1})).rc === 0, 'fresh module startup failed');
  await completion(control, true);
  report.reopen = await control.call('report');
  require(report.reopen.failures === 0, 'fresh module OPFS reopen failed');
  report.measurementScope = {
    bytes: 8192, callers: [1, 3], samplesPerCaller: 64, warmupPerCaller: 4,
    beforeOwner: 'WasmFS lock wait, proxy queue, dispatch and bridge setup together; not pure queue time.',
    ownerApi: 'Browser access method bracket with observation overhead; browser copying, caching and device work cannot be separated.',
    afterOwner: 'Bridge completion and proxy return to the C caller.',
    copy: 'Measured access read/write use shared Wasm heap views. Separate Uint8Array.set calibration does not imply zero browser copying. Readonly Blob-path costs are not benchmarked.',
    clocks: 'performance.timeOrigin + performance.now in each Worker. Independently coarsened clocks can yield small negative boundary deltas, retained without clamping; sub-resolution timings are inconclusive.',
    emptyProxy: 'Same SDK owner, separate Emscripten proxy queue; not the private WasmFS queue.',
    flush: 'Each fsync follows a fresh 8 KiB pwrite. Percentiles time fsync only; batch throughput includes dirtying writes. Not commit latency.',
    durability: 'Fresh module reopens after explicit flush and orderly close; no process crash, power loss, WAL or SQL proof.',
  };
  report.status = 'passed';
} catch (error) {
  report.error = String(error.stack || error);
} finally {
  control.close();
  window.probeResult = report;
  document.querySelector('#status').textContent = JSON.stringify(report);
}
