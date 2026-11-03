/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
import {createRequire} from 'node:module';
import {createServer} from 'node:http';
import {spawn, spawnSync} from 'node:child_process';
import {readFileSync, writeFileSync, appendFileSync, existsSync, mkdtempSync, rmSync} from 'node:fs';
import {resolve, join} from 'node:path';
import {tmpdir} from 'node:os';
import {parseArgs} from 'node:util';
import assert from 'node:assert/strict';
import {processes} from './processes.mjs';
const {values} = parseArgs({options: {root: {type: 'string'}, engine: {type: 'string', default: 'chromium'},
  executable: {type: 'string'}, output: {type: 'string'}, iterations: {type: 'string', default: '1000'},
  waves: {type: 'string', default: '3'}, idle: {type: 'string', default: '60'}, seconds: {type: 'string', default: '7200'}}});
const iterations = Number(values.iterations), waves = Number(values.waves), idle = Number(values.idle);
const seconds = Number(values.seconds), root = resolve(values.root), output = resolve(values.output);
assert(['chromium', 'firefox', 'webkit'].includes(values.engine));
assert(Number.isInteger(iterations) && iterations >= 1 && iterations <= 1000000);
assert(Number.isInteger(waves) && waves >= 2 && waves <= 10);
assert(Number.isInteger(idle) && idle >= 30 && idle <= 300);
assert(Number.isInteger(seconds) && seconds >= 1 && seconds <= 28800);
const require = createRequire(join(root, 'wasm/package.json'));
const bundled = require('playwright')[values.engine].executablePath();
const executable = values.executable || bundled;
const script = readFileSync(join(root, 'tests/wasm/soak/worker-memory.js'));
const report = {kind: 'postgamma.wasm-worker-memory-diagnostic', status: 'in_progress', productionAcceptance: false,
  engine: values.engine, executable, parameters: {iterations, waves, idle, seconds}, samples: [],
  scope: 'Ordinary module Workers only; no SDK, Wasm, PostgreSQL, OPFS, inspector, profiler or forced GC. '
    + 'Native browser allocator is unchanged. FinalizationRegistry observes JS wrappers only, not native VM destruction. '
    + 'Compare natural idle medians across waves; diagnostic completion does not qualify the original mixed-load gate.'};
const resources = output.replace(/\.json$/, '.resources.jsonl'), log = output.replace(/\.json$/, '.native.log');
writeFileSync(resources, ''); writeFileSync(log, '');
const save = () => writeFileSync(output, JSON.stringify(report, null, 2) + '\n');
const started = performance.now();
let browser, timer, monitor, lastProgress = started, finish, fail;
const complete = new Promise((resolve, reject) => {finish = resolve; fail = reject;});
function sample(event) {
  const row = {...event, elapsedSeconds: (performance.now() - started) / 1000, ...processes()};
  report.samples.push(row); appendFileSync(resources, JSON.stringify(row) + '\n'); save();
  const accounted = s => s.anonymousPssBytes + s.sharedMemoryPssBytes + s.swapPssBytes;
  assert(accounted(row) - accounted(report.samples[0]) < 1536 * 1048576, 'Diagnostic allocation growth exceeds 1.5 GiB');
}
const server = createServer(async (request, response) => {
  response.setHeader('Cache-Control', 'no-store');
  try {
    const path = new URL(request.url, 'http://localhost').pathname;
    if (request.method === 'POST') {
      assert.equal(path, '/__event');
      const chunks = []; let size = 0;
      for await (const chunk of request) {size += chunk.length; assert(size <= 8192); chunks.push(chunk);}
      const event = JSON.parse(Buffer.concat(chunks)); lastProgress = performance.now(); sample(event);
      assert.notEqual(event.phase, 'error', event.error);
      response.writeHead(204); response.end();
      if (event.phase === 'done') finish();
      return;
    }
    if (path === '/worker-memory.html') {
      response.setHeader('Content-Type', 'text/html');
      response.end('<!doctype html><meta charset="utf-8"><script type="module" src="/worker-memory.js"></script>');
    } else if (path === '/worker-memory.js' || path === '/idle-worker.js') {
      response.setHeader('Content-Type', 'text/javascript');
      response.end(path === '/worker-memory.js' ? script : 'postMessage({ready:true});');
    } else {response.writeHead(404); response.end();}
  } catch (error) {
    report.serverError = String(error.stack || error); response.writeHead(500); response.end('Diagnostic failed'); fail(error);
  }
});
const profile = mkdtempSync(join(tmpdir(), 'pgm-worker-memory-'));
try {
  assert(existsSync(executable), 'Browser executable does not exist: ' + executable);
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  const url = `http://127.0.0.1:${server.address().port}/worker-memory.html?iterations=${iterations}&waves=${waves}&idle=${idle}`;
  let webkitProfile = '--user-data-dir';
  if (values.engine === 'webkit') {
    const help = spawnSync(executable, ['--help'], {encoding: 'utf8', timeout: 30000});
    assert.equal(help.status, 0, String(help.error || help.stderr));
    if (!help.stdout.includes('--user-data-dir')) {
      assert(help.stdout.includes('--profile-dir'), 'Unsupported MiniBrowser profile option');
      webkitProfile = '--profile-dir';
    }
  }
  const argumentsByEngine = {
    chromium: ['--headless', '--no-sandbox', '--disable-dev-shm-usage', '--no-first-run',
      '--disable-background-networking', '--disable-component-update', '--disable-default-apps',
      '--disable-extensions', '--disable-sync', '--user-data-dir=' + profile, url],
    firefox: ['-headless', '-profile', profile, url],
    webkit: ['--headless', webkitProfile + '=' + profile, url],
  };
  report.command = [executable, ...argumentsByEngine[values.engine]]; save();
  browser = spawn(executable, report.command.slice(1), {stdio: ['ignore', 'pipe', 'pipe'],
    env: {...process.env, ...(values.engine === 'webkit' ? {WebKitPasStatusReporter: '2'} : {})}});
  browser.stdout.on('data', chunk => appendFileSync(log, chunk));
  browser.stderr.on('data', chunk => appendFileSync(log, chunk));
  browser.once('error', fail); browser.once('exit', code => fail(new Error('Browser exited: ' + code)));
  monitor = setInterval(() => {
    if (performance.now() - lastProgress > 90000) fail(new Error('No diagnostic progress within 90 s'));
  }, 1000);
  timer = setTimeout(() => fail(new Error('Worker diagnostic deadline exceeded')), seconds * 1000);
  await complete;
  const last = report.samples.at(-1);
  assert.equal(last.counts.created, iterations * waves * 14); assert.equal(last.counts.terminated, last.counts.created);
  const median = rows => [...rows].sort((a, b) => a - b)[Math.floor(rows.length / 2)];
  report.idleWaves = Array.from({length: waves}, (_, i) => {
    const rows = report.samples.filter(row => row.phase === 'idle' && row.wave === i + 1).slice(-3);
    assert.equal(rows.length, 3);
    return {wave: i + 1, counts: rows.at(-1).counts,
      accountedBytes: median(rows.map(row => row.anonymousPssBytes + row.sharedMemoryPssBytes + row.swapPssBytes)),
      rssBytes: median(rows.map(row => row.rssBytes)), threads: median(rows.map(row => row.threads))};
  });
  report.interWaveGrowthBytes = report.idleWaves.slice(1).map((row, i) => row.accountedBytes - report.idleWaves[i].accountedBytes);
  report.status = 'diagnostic-completed';
} catch (error) {report.status = 'failed'; report.error = String(error.stack || error); process.exitCode = 1;}
finally {
  clearInterval(monitor); clearTimeout(timer); report.elapsedSeconds = (performance.now() - started) / 1000; save();
  if (browser && browser.exitCode === null && browser.signalCode === null && browser.pid) {
    browser.kill('SIGTERM');
    await new Promise(resolve => {browser.once('exit', resolve); setTimeout(() => {browser.kill('SIGKILL'); resolve();}, 10000).unref();});
  }
  if (server.listening) await new Promise(resolve => server.close(resolve));
  rmSync(profile, {recursive: true, force: true});
  console.log(report.status + '; ' + output);
}
