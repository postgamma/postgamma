/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
window.probeResult = null;
const report = {status: 'failed', isolated: crossOriginIsolated, kernelValidated: false};
const require = (condition, message) => { if (!condition) throw new Error(message); };
const delay = (ms) => new Promise((resolve) => setTimeout(resolve, ms));
let worker;
try {
  require(crossOriginIsolated, 'Cross-origin isolation required');
  worker = new Worker('./resource-worker.mjs', {type: 'module'});
  const pending = new Map();
  let sequence = 0;
  Object.assign(report, await new Promise((resolve, reject) => {
    const timer = setTimeout(() => reject(new Error('Resource module startup timed out')), 10000);
    worker.onmessage = ({data}) => {
      if (data.kind === 'ready' || data.kind === 'fatal') {
        clearTimeout(timer);
        if (data.kind === 'fatal') reject(new Error(data.error)); else resolve(data);
      } else if (pending.has(data.id)) {
        if (data.error) pending.get(data.id).reject(new Error(data.error));
        else pending.get(data.id).resolve(data.result);
      }
    };
    worker.onerror = (error) => { clearTimeout(timer); reject(new Error(error.message)); };
  }));
  async function call(command) {
    const id = ++sequence;
    let timer;
    try {
      return await new Promise((resolve, reject) => {
        pending.set(id, {resolve, reject});
        timer = setTimeout(() => reject(new Error(`${command} timed out`)), 5000);
        worker.postMessage({id, command});
      });
    } finally { clearTimeout(timer); pending.delete(id); }
  }
  async function until(field, expected) {
    const deadline = performance.now() + 5000;
    while (performance.now() < deadline) {
      const value = await call('snapshot');
      if (value[field] === expected) return value;
      await delay(5);
    }
    throw new Error(`${field} did not reach ${expected}`);
  }
  report.budget = await call('snapshot');
  report.rounds = [];
  for (let round = 0; round < 2; round++) {
    require((await call('start')).rc === 0, 'Planned pthread capacity could not start');
    const before = await until('ready', report.budget.pthreadCount);
    let replies = 0;
    const start = performance.now();
    do {
      require((await call('snapshot')).done === 0, 'Workers returned before release');
      replies++;
      await delay(10);
    } while (performance.now() - start < 100);
    const growth = await call('grow');
    report.lastGrowth = growth;
    require(growth.afterBytes > 0 && growth.newRegionReadable && growth.allocationRejected &&
            growth.errno === growth.ENOMEM && growth.refreshedViewBytes === growth.afterBytes,
            'Growth, refreshed view, or bounded allocation failure check failed');
    if (round === 0) {
      require(growth.afterBytes > growth.beforeBytes && growth.oldViewBytes < growth.afterBytes,
              'First round did not exercise shared-memory growth');
      report.growth = growth;
    }
    await call('release');
    await until('done', report.budget.pthreadCount);
    const joined = await call('join');
    require(joined.failures === 0 && joined.threads.length === report.budget.pthreadCount &&
      joined.threads.every((t) => t.joined && t.failures === 0 && t.heapAfterBytes === growth.afterBytes &&
        t.stackBase - t.stackEnd === t.requestedStack) &&
      new Set(joined.threads.map((t) => t.tlsAddress)).size === joined.threads.length,
      'Thread stack, TLS, shared growth visibility or cleanup check failed');
    const after = await call('snapshot');
    require(after.allocatorLiveBytes === report.budget.allocatorLiveBytes && after.failures === 0,
            'Probe allocations were not released after join');
    report.rounds.push({before, growth, joined, controlReplies: replies, after});
  }
  report.status = 'passed';
} catch (error) { report.error = String(error.stack || error); }
finally {
  if (worker) worker.terminate();
  window.probeResult = report;
  document.querySelector('#status').textContent = JSON.stringify(report);
}
