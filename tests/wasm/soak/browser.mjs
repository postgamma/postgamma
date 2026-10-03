/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
import {createRequire} from 'node:module';
import {appendFileSync, readFileSync, readdirSync, writeFileSync} from 'node:fs';
import {resolve} from 'node:path';
import {parseArgs} from 'node:util';
import assert from 'node:assert/strict';
import {bounded, controlSnapshots, failureSnapshot} from '../browser/diagnostics.mjs';
import {resourceGate} from './resources.mjs';
const {values} = parseArgs({options: {root: {type: 'string'}, url: {type: 'string'}, output: {type: 'string'},
  profile: {type: 'string'}, engine: {type: 'string', default: 'chromium'}, executable: {type: 'string'},
  seconds: {type: 'string', default: '120'}, 'growth-mib': {type: 'string', default: '128'}}});
const require = createRequire(resolve(values.root, 'wasm/package.json'));
const engine = require('playwright')[values.engine];
const seconds = Number(values.seconds), growthBytes = Number(values['growth-mib']) * 1024 * 1024;
assert(Number.isInteger(seconds) && seconds >= 1 && seconds <= 28800);
assert(Number.isFinite(growthBytes) && growthBytes >= 0);
const report = {kind: 'postgamma.wasm-soak-browser', status: 'failed', engine: values.engine,
  seconds, growthBytes, rounds: 0, reopens: 0, samples: [], pageErrors: [], failedRequests: [], recentConsole: []};
const eventsFile = values.output.replace(/\.json$/, '.events.jsonl');
const resourcesFile = values.output.replace(/\.json$/, '.resources.jsonl');
writeFileSync(eventsFile, ''); writeFileSync(resourcesFile, '');
function processes() {
  const rows = new Map();
  for (const id of readdirSync('/proc').filter(name => /^\d+$/.test(name))) {
    try {
      const status = readFileSync(`/proc/${id}/status`, 'utf8');
      const field = name => Number(status.match(new RegExp('^' + name + ':\\s+(\\d+)', 'm'))?.[1] || 0);
      rows.set(Number(id), {pid: Number(id), parent: field('PPid'), rssBytes: field('VmRSS') * 1024, threads: field('Threads')});
    } catch { /* A process can exit between the directory and status reads. */ }
  }
  const selected = new Set([process.pid]);
  for (let changed = true; changed;) {
    changed = false;
    for (const row of rows.values()) if (selected.has(row.parent) && !selected.has(row.pid)) {selected.add(row.pid); changed = true;}
  }
  const result = {processes: 0, rssBytes: 0, pssBytes: 0, anonymousPssBytes: 0,
    sharedMemoryPssBytes: 0, filePssBytes: 0, swapPssBytes: 0, threads: 0, fileDescriptors: 0};
  for (const id of selected) {
    if (id === process.pid) continue; // The Node test driver is recorded separately.
    const row = rows.get(id); if (!row) continue;
    try {
      const rollup = readFileSync(`/proc/${id}/smaps_rollup`, 'utf8');
      const bytes = name => {
        const value = rollup.match(new RegExp('^' + name + ':\\s+(\\d+)', 'm'))?.[1];
        assert(value !== undefined, 'Missing browser process ' + name);
        return Number(value) * 1024;
      };
      const descriptors = readdirSync(`/proc/${id}/fd`).length;
      result.pssBytes += bytes('Pss'); result.swapPssBytes += bytes('SwapPss');
      result.anonymousPssBytes += bytes('Pss_Anon');
      result.sharedMemoryPssBytes += bytes('Pss_Shmem'); result.filePssBytes += bytes('Pss_File');
      result.processes++; result.rssBytes += row.rssBytes; result.threads += row.threads;
      result.fileDescriptors += descriptors;
    } catch (error) {
      if (['ENOENT', 'ESRCH'].includes(error.code)) continue; // Exited during sampling.
      throw error;
    }
  }
  assert(result.processes >= 1 && result.rssBytes > 0, 'Browser process accounting is available');
  return result;
}
let context, page, readyAt = 0, lastProgress = performance.now(), lastSample = 0, lastLog = 0;
const retiredTargets = new Map();
const metadata = url => new URLSearchParams(new URL(url).hash.slice(1));
let eventTail = Promise.resolve();
async function sample(event) {
  const ownership = await bounded(() => page.evaluate(() => window.soakWorkerOwnership()));
  assert.equal(ownership.active?.length, 1, 'Exactly one application-owned control Worker');
  const owned = worker => /\/src\/worker\/sql\.js(?:[?#]|$)/.test(worker.url()) || metadata(worker.url()).has('postgammaPthread');
  const all = page.workers().filter(worker => /\/src\/worker\/sql\.js(?:[?#]|$)/.test(worker.url()));
  const live = all.filter(worker => ownership.active.includes(metadata(worker.url()).get('soakControl')));
  assert.equal(ownership.activePthreads.length, 13, 'The API owner retains the complete fixed pthread pool');
  const livePthreads = page.workers().filter(worker => ownership.activePthreads.includes(metadata(worker.url()).get('postgammaPthread')));
  assert.equal(livePthreads.length, 13, 'Every page-owned pthread Worker has a live browser target');
  // Chromium can retain a debug target after Worker.terminate(). Track the
  // actual constructor/terminate calls; never evaluate a retired debug target.
  const controls = await controlSnapshots({workers: () => live});
  assert.equal(controls.length, 1, 'Exactly one live control Worker at a quiescent boundary');
  const value = controls[0].snapshot;
  assert(!value.error && value.pending <= 1, 'Control diagnostics are available');
  assert(value.memoryBytes <= 536870912 && value.threads.length <= 13, 'Native memory/thread limit');
  assert(value.storage?.store && value.storage.store.handles <= 63, 'OPFS owner handle bound');
  const now = performance.now(), retired = page.workers().filter(worker => owned(worker) && !live.includes(worker) && !livePthreads.includes(worker)).map(worker => worker.url());
  for (const url of retired) if (!retiredTargets.has(url)) retiredTargets.set(url, now);
  for (const url of retiredTargets.keys()) if (!retired.includes(url)) retiredTargets.delete(url);
  const retiredWorkers = [...retiredTargets].map(([url, at]) => ({url, ageMs: now - at}));
  const row = {elapsedSeconds: (performance.now() - readyAt) / 1000, iteration: event.iteration,
    phase: event.kind, cycleEnd: event.kind === 'round' && event.iteration % report.parameters.reopenEvery === 0,
    controlWorkersCreated: ownership.created, controlWorkersTerminated: ownership.terminated,
    pthreadWorkersCreated: ownership.pthreadCreated, pthreadWorkersTerminated: ownership.pthreadTerminated,
    retiredDebugTargets: retiredWorkers.length, retiredWorkers,
    retiredWorkerMaxAgeMs: Math.max(0, ...retiredWorkers.map(worker => worker.ageMs)),
    runnerMemory: process.memoryUsage(),
    ...processes(), linearMemoryBytes: value.memoryBytes, nativeThreads: value.threads.length,
    storageHandles: value.storage.store.handles, virtualOpens: value.storage.store.virtualOpens};
  appendFileSync(resourcesFile, JSON.stringify(row) + '\n');
  report.samples.push(row); lastSample = performance.now();
  const trend = resourceGate(report.samples, growthBytes);
  if (trend.status === 'failed') {
    report.resourceGate = trend;
    throw new Error('Live resource gate failed: ' + JSON.stringify(trend));
  }
}
try {
  context = await engine.launchPersistentContext(values.profile, {headless: true, timeout: 30000,
    ...(values.executable ? {executablePath: values.executable} : values.engine === 'chromium' ? {channel: 'chromium'} : {})});
  report.browserVersion = context.browser().version();
  page = context.pages()[0] || await context.newPage();
  await page.addInitScript(() => {
    const NativeWorker = window.Worker, active = new Set(), activePthreads = new Set();
    let created = 0, terminated = 0, pthreadCreated = 0, pthreadTerminated = 0;
    window.soakWorkerOwnership = () => ({active: [...active], created, terminated,
      activePthreads: [...activePthreads], pthreadCreated, pthreadTerminated});
    window.Worker = class extends NativeWorker {
      constructor(source, options) {
        const url = new URL(source, location.href);
        const id = /\/src\/worker\/sql\.js$/.test(url.pathname) ? String(++created) : undefined;
        if (id) url.hash = new URLSearchParams({soakControl: id}).toString();
        const pthreadId = new URLSearchParams(url.hash.slice(1)).get('postgammaPthread');
        super(url, options); this.soakId = id; this.pthreadId = pthreadId;
        if (id) active.add(id);
        if (pthreadId) {activePthreads.add(pthreadId); pthreadCreated++;}
      }
      terminate() {
        super.terminate();
        if (this.soakId && active.delete(this.soakId)) terminated++;
        if (this.pthreadId && activePthreads.delete(this.pthreadId)) pthreadTerminated++;
      }
    };
  });
  page.on('pageerror', error => report.pageErrors.push(String(error)));
  page.on('console', message => {report.recentConsole.push({type: message.type(), text: message.text().slice(0, 4000)}); if (report.recentConsole.length > 64) report.recentConsole.shift();});
  page.on('requestfailed', request => report.failedRequests.push({url: request.url(), error: request.failure()?.errorText}));
  page.on('response', response => {if (response.status() >= 400) report.failedRequests.push({url: response.url(), status: response.status()});});
  await page.exposeFunction('soakEvent', event => {
    const work = eventTail.then(async () => {
      lastProgress = performance.now();
      appendFileSync(eventsFile, JSON.stringify(event) + '\n');
      if (event.kind === 'ready') {readyAt = performance.now(); report.parameters = event;}
      if (event.kind === 'round') {report.rounds = event.iteration; report.commands = event.totalCommands; report.copyBytes = event.totalCopyBytes;}
      if (event.kind === 'reopen') {
        report.reopens++;
        if (event.fault) assert(event.metrics.storageReleased && event.metrics.runningWorkers === 0);
        else {
          assert.deepEqual(event.metrics.resources, {locks: 0, shmem: 0, dsm: 0, pipes: 0});
          assert.equal(event.metrics.runningWorkers, 0);
        }
      }
      if (['ready', 'round', 'reopen'].includes(event.kind) &&
          (performance.now() - lastSample > 10000 || event.kind === 'round' && event.iteration % report.parameters.reopenEvery === 0)) await sample(event);
      if (event.kind === 'complete') report.complete = event;
      if (performance.now() - lastLog > 60000) {
        console.log(JSON.stringify({engine: values.engine, rounds: report.rounds, reopens: report.reopens,
          elapsedSeconds: readyAt ? Math.round((performance.now() - readyAt) / 1000) : 0, resources: report.samples.at(-1)}));
        lastLog = performance.now();
      }
      writeFileSync(values.output, JSON.stringify(report, null, 2) + '\n');
    });
    eventTail = work.catch(() => {}); return work;
  });
  await page.goto(values.url, {timeout: 30000});
  const replayRounds = Number(new URL(values.url).searchParams.get('rounds') || 0);
  const deadline = performance.now() + Math.max(seconds + 600, replayRounds * 30 + 600) * 1000;
  for (;;) {
    const result = await bounded(() => page.evaluate(() => window.probeResult), 10000);
    assert(!result?.error || result.status, result?.error);
    if (result) {report.probe = result; break;}
    assert(performance.now() < deadline, 'Soak total deadline');
    assert(performance.now() - lastProgress < (readyAt ? 90000 : 180000), 'Soak progress deadline');
    await new Promise(resolve => setTimeout(resolve, 1000));
  }
  await eventTail;
  assert.equal(report.probe.status, 'passed', report.probe.error);
  assert(report.complete && report.complete.crashes > 0 && report.complete.orderly > 0 && report.complete.cancellations > 0, 'Complete recovery and cancellation evidence');
  if (!report.parameters.rounds) assert(report.complete.elapsedMs >= seconds * 1000, 'Requested duration completed');
  assert.deepEqual(report.pageErrors, []); assert.deepEqual(report.failedRequests, []);
  const finalMetrics = report.probe.metrics.at(-1);
  assert.deepEqual(finalMetrics.resources, {locks: 0, shmem: 0, dsm: 0, pipes: 0}); assert.equal(finalMetrics.runningWorkers, 0);
  report.workerOwnership = await bounded(() => page.evaluate(() => window.soakWorkerOwnership()));
  assert.deepEqual(report.workerOwnership.active, [], 'Final close retires every application-owned control Worker');
  assert.equal(report.workerOwnership.created, report.workerOwnership.terminated);
  assert.deepEqual(report.workerOwnership.activePthreads, [], 'Final close retires every application-owned pthread Worker');
  assert.equal(report.workerOwnership.pthreadCreated, report.workerOwnership.pthreadTerminated);
  const physicallyClosed = await bounded(async () => {
    while (page.workers().some(worker => /\/src\/worker\/sql\.js(?:[?#]|$)/.test(worker.url()) || metadata(worker.url()).has('postgammaPthread')))
      await new Promise(resolve => setTimeout(resolve, 10));
    return true;
  }, 10000);
  assert.equal(physicallyClosed, true, 'Final control and pthread Workers disappear within ten seconds');
  report.resourceGate = resourceGate(report.samples, growthBytes);
  assert.notEqual(report.resourceGate.status, 'failed', JSON.stringify(report.resourceGate));
  report.status = 'passed';
} catch (error) {
  report.error = String(error.stack || error);
  if (report.resourceGate?.status === 'passed') report.resourceGate.status = 'failed';
  if (page && !page.isClosed()) {
    report.failureSnapshot = await failureSnapshot(page);
    report.active = await bounded(() => page.evaluate(() => window.soakState));
  }
  process.exitCode = 1;
} finally {
  report.eventsFile = eventsFile; report.resourcesFile = resourcesFile;
  report.measurementScope = 'Linux browser descendant RSS excludes the Node runner and includes shared pages in multiple processes. Anonymous and shared-memory PSS plus SwapPss gate allocated memory including swapped pages. Total/file PSS remain recorded: shared executable pages can be redistributed when unrelated browsers exit. Runner RSS/heap/external bytes are recorded separately. Retired control and pthread targets must disappear within ten seconds. Quiescent samples include one live database. Budgets detect regressions, not a proof of zero leaks.';
  writeFileSync(values.output, JSON.stringify(report, null, 2) + '\n');
  await context?.close();
}
