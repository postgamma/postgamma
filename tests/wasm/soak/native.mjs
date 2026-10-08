/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
// Use the browser's native worker lifetime and a read-only application relay.
// Launch Firefox and WPE directly; application events and OS threads provide
// evidence without an automation response-body collector or worker inspector.
import {createServer} from 'node:http';
import {spawn} from 'node:child_process';
import {readFileSync, writeFileSync, appendFileSync, readdirSync, mkdirSync} from 'node:fs';
import {resolve, join, dirname, extname, sep} from 'node:path';
import {fileURLToPath} from 'node:url';
import {createRequire} from 'node:module';
import {parseArgs} from 'node:util';
import {createHash} from 'node:crypto';
import assert from 'node:assert/strict';
import {workerObserver} from './observer.mjs';
const {values} = parseArgs({options: {root: {type: 'string'}, site: {type: 'string'}, url: {type: 'string'},
  executable: {type: 'string'}, output: {type: 'string'}, profile: {type: 'string'},
  engine: {type: 'string', default: 'webkit'}, seconds: {type: 'string'}, 'growth-mib': {type: 'string', default: '128'},
  'idle-seconds': {type: 'string', default: '60'}}});
const root = resolve(values.root), site = resolve(values.site), output = resolve(values.output);
const folder = dirname(fileURLToPath(import.meta.url));
const {processes} = await import('./processes.mjs');
const {resourceGate} = await import('./resources.mjs');
const require = createRequire(join(root, 'wasm/package.json'));
const {wsServer: WebSocketServer} = require(join(dirname(require.resolve('playwright-core/package.json')), 'lib/utilsBundle.js'));
const seconds = Number(values.seconds), growthBytes = Number(values['growth-mib']) * 1048576;
assert(Number.isInteger(seconds) && seconds >= 1 && seconds <= 28800);
assert(Number.isFinite(growthBytes) && growthBytes >= 0);
const idleSeconds = Number(values['idle-seconds']);
assert(Number.isInteger(idleSeconds) && idleSeconds >= 0 && idleSeconds <= 120);
assert.equal(process.platform, 'linux', 'Native Worker thread accounting requires Linux');
assert(['webkit', 'firefox'].includes(values.engine));
const engine = values.engine, driver = engine === 'webkit' ? 'native-wpe' : 'native-firefox';
const originalURL = new URL(values.url), rounds = Number(originalURL.searchParams.get('rounds') || 0);
const eventsFile = output.replace(/\.json$/, '.events.jsonl'), resourcesFile = output.replace(/\.json$/, '.resources.jsonl');
writeFileSync(eventsFile, '');writeFileSync(resourcesFile, '');
const report = {kind: 'postgamma.wasm-soak-browser', status: 'in_progress', engine, driver,
  seconds, growthBytes, rounds: 0, reopens: 0, samples: [], pageErrors: [], failedRequests: []};
const observer = readFileSync(join(folder, 'native-relay.js'));
const sha = bytes => createHash('sha256').update(bytes).digest('hex');
report.observerSources = {relay: sha(observer), workerPrefix: sha(workerObserver), driver: sha(readFileSync(fileURLToPath(import.meta.url)))};
report.browserProtocolCommands = [];
const started = performance.now();let child, readyAt, lastProgress = started, extraThreadsAt;
let resolveComplete, rejectComplete, tail = Promise.resolve();
const complete = new Promise((resolve, reject) => {resolveComplete = resolve;rejectComplete = reject;});
void complete.catch(() => {}); // Setup can fail before the completion await.
const save = () => writeFileSync(output, JSON.stringify(report, null, 2) + '\n');
function workerThreads(row) {
  let count = 0;
  for (const process of row.processRows) {
    const command = process.command.join(' ');
    if (engine === 'webkit' ? !process.command.some(arg => arg.endsWith('/WPEWebProcess')) :
        !command.includes('-contentproc') || !command.includes('-isForBrowser')) continue;
    let tids;
    try {tids = readdirSync(`/proc/${process.pid}/task`);} catch (error) {if (error.code === 'ENOENT') continue;throw error;}
    for (const id of tids) {
      try {if (readFileSync(`/proc/${process.pid}/task/${id}/comm`, 'utf8').trim() === (engine === 'webkit' ? 'WebCore: Worker' : 'DOM Worker')) count++;}
      catch (error) {if (!['ENOENT', 'ESRCH'].includes(error.code)) throw error;}
    }
  }
  return count;
}
async function eventReceived(event) {
  lastProgress = performance.now();appendFileSync(eventsFile, JSON.stringify(event) + '\n');
  if (event.kind === 'page-error') {report.pageErrors.push(event.error);throw new Error(event.error);}
  if (event.kind === 'ready') {readyAt = performance.now();report.parameters = event;report.userAgent = event.userAgent;}
  if (event.kind === 'round') {report.rounds = event.iteration;report.commands = event.totalCommands;report.copyBytes = event.totalCopyBytes;}
  if (event.kind === 'reopen') {
    report.reopens++;
    if (event.fault) assert(event.metrics.storageReleased && event.metrics.runningWorkers === 0);
    else {assert.deepEqual(event.metrics.resources, {locks: 0, shmem: 0, dsm: 0, pipes: 0});assert.equal(event.metrics.runningWorkers, 0);}
  }
  if (event.controlSnapshot) {
    assert.equal(event.ownership.nativeOwnerPosts, 0, 'SDK messages use child-owned ports rather than leaking native owner posts');
    assert.equal(event.ownership.active.length, 1);assert.equal(event.ownership.activePthreads.length, 13);
    const value = event.controlSnapshot;
    assert(!value.error && value.pending <= 1, 'Healthy quiescent control snapshot');
    assert(value.memoryBytes <= 536870912 && value.threads.length <= 13, 'Native memory/thread bounds');
    assert(value.storage?.store && value.storage.store.handles <= 63, 'OPFS owner handle bound');
    const resource = processes(), physical = workerThreads(resource), expected = event.ownership.active.length + event.ownership.activePthreads.length;
    assert(physical >= expected, 'Every owned Worker has a native thread');
    const now = performance.now();if (physical > expected) extraThreadsAt ??= now;else extraThreadsAt = undefined;
    const row = {elapsedSeconds: (now - readyAt) / 1000, iteration: event.iteration, phase: event.kind,
      cycleEnd: event.kind === 'round' && event.iteration % report.parameters.reopenEvery === 0,
      ...resource, linearMemoryBytes: value.memoryBytes, nativeThreads: value.threads.length,
      storageHandles: value.storage.store.handles, virtualOpens: value.storage.store.virtualOpens,
      physicalWorkerThreads: physical, retiredWorkerMaxAgeMs: extraThreadsAt === undefined ? 0 : now - extraThreadsAt,
      runnerMemory: process.memoryUsage(), controlWorkersCreated: event.ownership.created,
      controlWorkersTerminated: event.ownership.terminated, pthreadWorkersCreated: event.ownership.pthreadCreated,
      pthreadWorkersTerminated: event.ownership.pthreadTerminated};
    report.samples.push(row);appendFileSync(resourcesFile, JSON.stringify(row) + '\n');
    report.resourceGate = resourceGate(report.samples, growthBytes);
    assert.notEqual(report.resourceGate.status, 'failed', 'Live resource gate: ' + JSON.stringify(report.resourceGate));
    save();
  }
  if (performance.now() - lastLog > 60000) {
    console.log(JSON.stringify({engine, driver, rounds: report.rounds, reopens: report.reopens,
      elapsedSeconds: readyAt ? Math.round((performance.now() - readyAt) / 1000) : 0, resourceGate: report.resourceGate}));
    lastLog = performance.now();
  }
  if (event.kind === 'complete') report.complete = event;
  if (event.kind === 'result') {
    report.probe = event.result;assert.equal(event.result.status, 'passed', event.result.error);
    assert(report.complete?.crashes > 0 && report.complete.orderly > 0 && report.complete.cancellations > 0);
    if (!rounds) assert(report.complete.elapsedMs >= seconds * 1000);else assert.equal(report.complete.iteration, rounds);
    const metrics = event.result.metrics.at(-1);
    assert.deepEqual(metrics.resources, {locks: 0, shmem: 0, dsm: 0, pipes: 0});assert.equal(metrics.runningWorkers, 0);
    const owner = event.ownership;assert.deepEqual(owner.active, []);assert.deepEqual(owner.activePthreads, []);
    assert.equal(owner.nativeOwnerPosts, 0);
    assert.equal(owner.created, owner.terminated);assert.equal(owner.pthreadCreated, owner.pthreadTerminated);
    report.workerOwnership = owner;
    const deadline = performance.now() + 10000;
    while (workerThreads(processes())) {
      assert(performance.now() < deadline, 'Final native Worker threads retire within ten seconds');
      await new Promise(resolve => setTimeout(resolve, 10));
    }
    report.finalPhysicalWorkerThreads = 0;
    assert.deepEqual(report.pageErrors, []);assert.deepEqual(report.failedRequests, []);
    report.resourceGate = resourceGate(report.samples, growthBytes);assert.notEqual(report.resourceGate.status, 'failed');
    report.phase = 'workload-completed';resolveComplete();
  }
}
const server = createServer((request, response) => {
  response.setHeader('Cross-Origin-Opener-Policy', 'same-origin');response.setHeader('Cross-Origin-Embedder-Policy', 'require-corp');
  response.setHeader('Cross-Origin-Resource-Policy', 'same-origin');response.setHeader('Cache-Control', 'no-store');
  const path = new URL(request.url, 'http://localhost').pathname;
  try {
    if (path === '/__native-soak.js') {response.setHeader('Content-Type', 'text/javascript');response.end(observer);return;}
    if (path === '/__control-probe.js') {
      const source = new URL(request.url, 'http://localhost').searchParams.get('source');
      assert(/^\/src\/worker\/sql\.js#soakControl=\d+$/.test(source));
      response.setHeader('Content-Type', 'text/javascript');
      response.end(workerObserver + '\nimport ' + JSON.stringify(source) + ';\n');return;
    }
    if (path === '/favicon.ico') {response.writeHead(204);response.end();return;}
    const file = resolve(site, '.' + decodeURIComponent(path));assert(file.startsWith(site + sep));
    const data = readFileSync(file);
    response.setHeader('Content-Type', ({'.html':'text/html', '.js':'text/javascript', '.mjs':'text/javascript', '.wasm':'application/wasm', '.json':'application/json'})[extname(file)] || 'application/octet-stream');
    if (path === '/sql.html') response.end(data.toString().replace('<script type="module"', '<script src="/__native-soak.js"></script>\n<script type="module"'));
    else response.end(data);
  } catch (error) {report.failedRequests.push({url:path, error:String(error)});response.writeHead(404);response.end('Failed test asset');rejectComplete(error);}
});
const sockets = new WebSocketServer({server, path: '/__soak', maxPayload: 2097152});
let connected = false;
sockets.on('connection', socket => {
  if (connected) {socket.close();rejectComplete(new Error('Multiple observer connections'));return;}
  connected = true;
  socket.on('message', data => {
    let message;
    try {message = JSON.parse(data);}catch (error) {rejectComplete(error);return;}
    const work = tail.then(() => eventReceived(message.event));
    tail = work.catch(error => {report.error = String(error.stack || error);rejectComplete(error);});
    work.then(() => socket.send(JSON.stringify({sequence:message.sequence})), error => socket.send(JSON.stringify({sequence:message.sequence,error:String(error)})));
  });
  socket.on('error', rejectComplete);socket.on('close', () => {if (report.status !== 'passed') rejectComplete(new Error('Observer disconnected'));});
});
let monitor, timer, lastLog = started;
try {
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  originalURL.host = '127.0.0.1:' + server.address().port;
  mkdirSync(values.profile, {recursive: true});
  const log = data => appendFileSync(output.replace(/\.json$/, '.native.log'), data);
  if (engine === 'firefox') {
    const executable = values.executable || require('playwright').firefox.executablePath();
    report.command = [executable, '--no-remote', '--headless', '--profile', values.profile, originalURL.href];
  } else {
    report.command = [values.executable, '--headless', '--profile-dir=' + values.profile, originalURL.href];
  }
  report.browserExecutable = {path: report.command[0], sha256: sha(readFileSync(report.command[0]))};
  child = spawn(report.command[0], report.command.slice(1), {stdio:['ignore','pipe','pipe'], detached: true});
  for (const stream of [child.stdout, child.stderr]) stream.on('data', log);
  save();
  child.once('error', rejectComplete);child.once('exit', code => rejectComplete(new Error('Browser exited: ' + code)));
  monitor = setInterval(() => {if (performance.now() - lastProgress > (readyAt ? 90000 : 180000)) rejectComplete(new Error('Soak progress deadline'));}, 1000);
  timer = setTimeout(() => rejectComplete(new Error('Soak total deadline')), Math.max(seconds + 600, rounds * 30 + 600) * 1000);
  await complete;await tail;
  clearInterval(monitor);
  report.phase = 'final-idle';save();
  const idleStarted = performance.now();
  await new Promise(resolve => setTimeout(resolve, idleSeconds * 1000));
  assert.equal(child.exitCode, null, 'Browser remains alive through final idle');
  assert.equal(child.signalCode, null, 'Browser remains alive through final idle');
  const finalResources = processes();
  assert.equal(workerThreads(finalResources), 0, 'Retired native Workers stay retired through final idle');
  report.finalIdle = {seconds: idleSeconds, elapsedSeconds: (performance.now() - idleStarted) / 1000,
    resources: finalResources, physicalWorkerThreads: 0};
  report.status = 'passed';report.phase = 'completed';
} catch (error) {report.status = 'failed';report.error = String(error.stack || error);process.exitCode = 1;}
finally {
  clearInterval(monitor);clearTimeout(timer);report.elapsedSeconds = (performance.now() - started) / 1000;
  report.eventsFile = eventsFile;report.resourcesFile = resourcesFile;
  report.measurementScope = (engine === 'webkit' ? 'Pinned upstream native WPE; ' : 'Native Firefox CLI with no automation protocol or response-body collector; ')
    + 'No inspector, profiler, forced GC or allocator override. '
    + 'Same workload and control/OPFS diagnostics,128MiB process budget,90s progress deadline. '
    + 'Linux OS Worker threads replace retired inspector targets; final Worker threads must retire within10s. '
    + 'An observer bootstrap imports the integrity-verified original control module and dispatches its existing read-only diagnostic. '
    + 'Every original SDK/kernel asset retains its original bytes and manifest checksum.';
  save();
  if (child && child.exitCode === null && child.signalCode === null) {
    try {process.kill(-child.pid, 'SIGTERM');} catch (error) {if (error.code !== 'ESRCH') throw error;}
    await new Promise(resolve => {
      const stop = setTimeout(() => {try {process.kill(-child.pid, 'SIGKILL');} catch (error) {if (error.code !== 'ESRCH') throw error;}resolve();}, 10000);
      child.once('exit', () => {clearTimeout(stop);resolve();});
    });
  }
  if (child) try {process.kill(-child.pid, 'SIGKILL');} catch (error) {if (error.code !== 'ESRCH') throw error;}
  for (const client of sockets.clients) client.terminate();
  await new Promise(resolve => sockets.close(resolve));await new Promise(resolve => server.close(resolve));
  console.log(report.status + '; ' + output);
}
