/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
import {createRequire} from 'node:module';
import {appendFileSync, writeFileSync} from 'node:fs';
import {resolve} from 'node:path';
import {parseArgs} from 'node:util';
import assert from 'node:assert/strict';
import {bounded, controlSnapshots, failureSnapshot} from '../browser/diagnostics.mjs';
import {resourceGate} from './resources.mjs';
import {MemoryDiagnostics} from './memory.mjs';
import {processes} from './processes.mjs';
import {workerObserver} from './observer.mjs';
const {values} = parseArgs({options: {root: {type: 'string'}, url: {type: 'string'}, output: {type: 'string'},
  profile: {type: 'string'}, engine: {type: 'string', default: 'chromium'}, executable: {type: 'string'},
  seconds: {type: 'string', default: '120'}, 'growth-mib': {type: 'string', default: '128'},
  'memory-diagnostics': {type: 'boolean', default: false}}});
const require = createRequire(resolve(values.root, 'wasm/package.json'));
const engine = require('playwright')[values.engine];
const seconds = Number(values.seconds), growthBytes = Number(values['growth-mib']) * 1024 * 1024;
assert(Number.isInteger(seconds) && seconds >= 1 && seconds <= 28800);
assert(Number.isFinite(growthBytes) && growthBytes >= 0);
const report = {kind: 'postgamma.wasm-soak-browser', status: 'in_progress', engine: values.engine,
  seconds, growthBytes, rounds: 0, reopens: 0, samples: [], pageErrors: [], failedRequests: [], recentConsole: []};
if (values['memory-diagnostics']) report.kind = 'postgamma.wasm-memory-diagnostic';
const eventsFile = values.output.replace(/\.json$/, '.events.jsonl');
const resourcesFile = values.output.replace(/\.json$/, '.resources.jsonl');
writeFileSync(eventsFile, ''); writeFileSync(resourcesFile, '');

let context, page, memory, readyAt = 0, lastProgress = performance.now(), lastSample = 0, lastLog = 0;
const retiredTargets = new Map();
const metadata = url => new URLSearchParams(new URL(url).hash.slice(1));
let eventTail = Promise.resolve();
async function sample(event) {
  const ownership = await bounded(() => page.evaluate(() => window.soakWorkerOwnership()));
  assert.equal(ownership.active?.length, 1, 'Exactly one application-owned control Worker');
  const owned = worker => /\/src\/worker\/sql\.js(?:[?#]|$)/.test(worker.url()) || metadata(worker.url()).has('postgammaPthread');
  const all = page.workers().filter(worker => /\/src\/worker\/sql\.js(?:[?#]|$)/.test(worker.url()));
  const live = all.filter(worker => ownership.active.includes(metadata(worker.url()).get('soakControl')));
  assert.equal(ownership.activePthreads.length, 14, 'The API owner retains the complete fixed pthread pool');
  const livePthreads = page.workers().filter(worker => ownership.activePthreads.includes(metadata(worker.url()).get('postgammaPthread')));
  assert.equal(livePthreads.length, 14, 'Every page-owned pthread Worker has a live browser target');
  // Chromium can retain a debug target after Worker.terminate(). Track the
  // actual constructor/terminate calls; never evaluate a retired debug target.
  const controls = await controlSnapshots({workers: () => live});
  assert.equal(controls.length, 1, 'Exactly one live control Worker at a quiescent boundary');
  const value = controls[0].snapshot;
  assert(!value.error && value.pending <= 1, 'Control diagnostics are available');
  assert(value.memoryBytes <= 536870912 && value.threads.length <= 14, 'Native memory/thread limit');
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
  if (memory && (event.kind === 'ready' || row.cycleEnd || event.kind === 'round' && event.iteration % 500 === 0))
    row.memoryDiagnostic = await memory.sample(event.iteration ?? 0);
  appendFileSync(resourcesFile, JSON.stringify(row) + '\n');
  report.samples.push(row); lastSample = performance.now();
  const trend = resourceGate(report.samples, growthBytes);
  if (trend.status === 'failed') {
    report.resourceGate = trend;
    if (!memory) throw new Error('Live resource gate failed: ' + JSON.stringify(trend));
    report.firstResourceFailure ??= {iteration: event.iteration, ...trend};
  }
}
try {
  context = await engine.launchPersistentContext(values.profile, {headless: true, timeout: 30000,
    ...(values.executable ? {executablePath: values.executable} : values.engine === 'chromium' ? {channel: 'chromium'} : {})});
  report.browserVersion = context.browser().version();
  page = context.pages()[0] || await context.newPage();
  if (values['memory-diagnostics']) {
    assert.equal(values.engine, 'chromium', 'CDP heap diagnostics require Chromium');
    memory = await MemoryDiagnostics.open(context, page, values.output);
  }
  const production = new URL(values.url).searchParams.get('faultSource') === 'control-worker';
  if (production) await context.route('**/src/worker/sql.js?soakObserver=*', route => {
    const url = new URL(route.request().url());
    const id = url.searchParams.get('soakObserver');
    assert(/^\d+$/.test(id));
    // The observing module is still a Worker of an isolated page. Keep the
    // server's policy on the synthetic response so Chromium can create it.
    return route.fulfill({contentType: 'text/javascript', headers: {
      'Cross-Origin-Embedder-Policy': 'require-corp',
      'Cross-Origin-Opener-Policy': 'same-origin',
      'Cache-Control': 'no-store',
    }, body: workerObserver +
      '\nimport ' + JSON.stringify(url.pathname + '#soakControl=' + id) + ';\n'});
  });
  await page.addInitScript(production => {
    const NativeWorker = window.Worker, active = new Set(), activePthreads = new Set(), controls = new Map();
    let created = 0, terminated = 0, pthreadCreated = 0, pthreadTerminated = 0;
    window.soakWorkerOwnership = () => ({active: [...active], created, terminated,
      activePthreads: [...activePthreads], pthreadCreated, pthreadTerminated});
    window.Worker = class extends NativeWorker {
      constructor(source, options) {
        const url = new URL(source, location.href);
        const id = /\/src\/worker\/sql\.js$/.test(url.pathname) ? String(++created) : undefined;
        if (id) url.hash = new URLSearchParams({soakControl: id}).toString();
        const pthreadId = new URLSearchParams(url.hash.slice(1)).get('postgammaPthread');
        const target = new URL(url);
        if (production && id) target.searchParams.set('soakObserver', id);
        super(target, options); this.soakId = id; this.pthreadId = pthreadId;
        if (id) {active.add(id);controls.set(id, this);}
        if (pthreadId) {activePthreads.add(pthreadId); pthreadCreated++;}
      }
      terminate() {
        super.terminate();
        if (this.soakId && active.delete(this.soakId)) {terminated++;controls.delete(this.soakId);}
        if (this.pthreadId && activePthreads.delete(this.pthreadId)) pthreadTerminated++;
      }
    };
    window.soakCrash = fault => {
      if (controls.size !== 1 || ![1, 2].includes(fault)) throw new Error('Invalid production fault ownership');
      const [id, worker] = controls.entries().next().value;
      return new Promise((resolve, reject) => {
        const timer = setInterval(() => {if (!controls.has(id)) {clearInterval(timer);clearTimeout(deadline);resolve();}}, 10);
        const deadline = setTimeout(() => {clearInterval(timer);reject(new Error('Production Worker cleanup deadline'));}, 10000);
        worker.postMessage({postgammaSoakFault: fault});
      });
    };
  }, production);
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
          (performance.now() - lastSample > 10000 || event.kind === 'round' &&
            (event.iteration % report.parameters.reopenEvery === 0 || memory && event.iteration % 500 === 0))) await sample(event);
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
  if (memory) {
    report.memoryFinal = await memory.finish();
    report.status = 'diagnostic-completed';
  } else {
    assert.notEqual(report.resourceGate.status, 'failed', JSON.stringify(report.resourceGate));
    report.status = 'passed';
  }
} catch (error) {
  report.status = 'failed';
  report.error = String(error.stack || error);
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
