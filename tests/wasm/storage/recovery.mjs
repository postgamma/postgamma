/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
import {createRequire} from 'node:module';
import {readFileSync, writeFileSync} from 'node:fs';
import {resolve} from 'node:path';
import {parseArgs} from 'node:util';
import assert from 'node:assert/strict';
const {values} = parseArgs({options: Object.fromEntries(['root', 'url', 'output', 'profile', 'executable', 'case', 'method']
  .map(key => [key, {type: 'string'}]))});
const require = createRequire(resolve(values.root, 'wasm/package.json'));
const {chromium} = require('playwright');
const report = {kind: 'postgamma.wasm-storage-recovery', status: 'failed', cases: [], console: [], pageErrors: [], failedRequests: [],
  profile: values.profile, url: values.url, playwright: require('playwright/package.json').version,
  interruption: 'Worker.terminate or SIGKILL of the owned browser process group; no graceful close at fault gates',
  hardwarePowerLossVerified: false};
let context, page;
const observedPages = new WeakSet();
async function workers() {
  const session = await context.browser().newBrowserCDPSession();
  try {return (await session.send('Target.getTargets')).targetInfos.filter(t => t.type === 'worker');}
  finally {await session.detach();}
}
const save = () => writeFileSync(values.output, JSON.stringify(report, null, 2) + '\n');
const invoke = async (method, ...args) => {
  let timer;
  try {
    return await Promise.race([page.evaluate(({method, args}) => window.recovery[method](...args), {method, args}),
      new Promise((_, reject) => {timer = setTimeout(() => reject(new Error('Recovery operation timed out: ' + method)), 90000);})]);
  } finally {clearTimeout(timer);}
};
const receiptTimer = setInterval(save, 2000);
async function load() {
  page = context.pages()[0] || await context.newPage();
  if (!observedPages.has(page)) {
    observedPages.add(page);
  page.on('console', message => report.console.push({type: message.type(), text: message.text()}));
  page.on('pageerror', error => report.pageErrors.push(String(error)));
  page.on('requestfailed', request => report.failedRequests.push({url: request.url(), error: request.failure()?.errorText}));
  page.on('response', response => {if (response.status() >= 400) report.failedRequests.push({url: response.url(), status: response.status()});});
  }
  await page.goto(values.url, {timeout: 15000});
  await page.waitForFunction(() => window.recovery, undefined, {timeout: 15000});
}
async function launch() {
  context = await chromium.launchPersistentContext(values.profile, {headless: true, timeout: 15000,
    ...(values.executable ? {executablePath: values.executable} : {channel: 'chromium'})});
  report.browserVersion = context.browser().version();
  await load();
}
async function killBrowser() {
  const browser = context.browser(), cdp = await browser.newBrowserCDPSession();
  const processes = (await cdp.send('SystemInfo.getProcessInfo')).processInfo;
  const pid = processes.find(p => p.type === 'browser')?.id;
  assert(Number.isSafeInteger(pid) && pid > 1, 'Own browser PID is available through its CDP session');
  const cmdline = readFileSync(`/proc/${pid}/cmdline`, 'utf8').split('\0');
  assert(cmdline.includes('--user-data-dir=' + values.profile), 'Browser PID belongs to this test profile');
  const stat = readFileSync(`/proc/${pid}/stat`, 'utf8');
  const fields = stat.slice(stat.lastIndexOf(')') + 2).split(' ');
  assert.equal(Number(fields[2]), pid, 'Playwright browser is the leader of its own process group');
  const disconnected = new Promise(resolve => browser.once('disconnected', resolve));
  process.kill(-pid, 'SIGKILL');
  await Promise.race([disconnected, new Promise((_, reject) => setTimeout(() => reject(new Error('Browser did not exit')), 15000).unref())]);
  context = undefined;
  return {method: 'SIGKILL', pid, processGroup: pid, cmdline, observedProcesses: processes, gracefulClose: false};
}
try {
  const name = 'recovery-' + crypto.randomUUID(), acknowledged = [];
  report.database = name;
  await launch();
  const startupOnly = values.case === 'startup';
  if (!startupOnly) {
    report.initialization = await invoke('initialize', name);
    assert(report.initialization.isolated);
  }
  const boundaries = startupOnly ? [] : values.case ? values.case.split(',') : ['wal-before', 'wal-after', 'checkpoint', 'create', 'extension',
    'rename-written', 'rename-flushed', 'journal-tail', 'compact-before', 'compact-prepared',
    'compact-partial', 'compact-written', 'compact-flushed', 'compact-retired', 'compact-published'];
  const methods = values.method ? [values.method] : ['worker', 'browser'];
  for (const method of methods) {
    assert(['worker', 'browser'].includes(method));
    for (const boundary of boundaries) {
      const id = method + '_' + boundary.replaceAll('-', '_');
      const record = {id, method, boundary, status: 'failed'};
      report.cases.push(record); save();
      console.log('storage recovery: ' + id);
      record.gate = await invoke('arm', id, boundary); acknowledged.push(id);
      record.workersBefore = await workers();
      record.beforeEvents = await invoke('events'); save();
      record.interruption = method === 'worker' ? await invoke('terminate') : await killBrowser();
      if (method === 'worker') record.workersAfter = await workers();
      record.stage = 'relaunch'; save();
      if (method === 'browser') await launch();
      const consoleStart = report.console.length;
      record.stage = 'open'; save();
      record.open = await invoke('open', name);
      record.workersReopened = await workers();
      record.stage = 'verify'; save();
      record.recovered = await invoke('verify', acknowledged, id, boundary);
      record.close = await invoke('close');
      record.workersClosed = await workers();
      assert(report.console.slice(consoleStart).some(e => e.text.includes('automatic recovery in progress')), 'PostgreSQL performs crash recovery');
      if (boundary === 'journal-tail') assert(record.open.mount.metrics.discardedJournalBytes > 0, 'Physical torn journal tail was discarded');
      record.status = 'passed'; save();
      await invoke('open', name);
    }
  }
  if (!startupOnly) report.finalClose = await invoke('close');
  // Interrupt target initdb itself, before a database has been published.
  for (const method of methods) {
    const record = {id: method + '_startup', method, boundary: 'startup', status: 'failed'};
    report.cases.push(record);
    const interrupted = 'startup-' + crypto.randomUUID();
    record.database = interrupted; record.gate = await invoke('startupGate', interrupted); save();
    record.interruption = method === 'worker' ? await invoke('startupTerminate') : await killBrowser();
    record.stage = 'relaunch'; save();
    if (method === 'browser') await launch(); else await load();
    record.stage = 'open'; save();
    record.open = await invoke('open', interrupted, true);
    record.stage = 'verify'; save();
    record.recovered = await invoke('startupVerify'); assert.equal(record.recovered.value, '23');
    record.close = await invoke('close'); record.status = 'passed'; save();
  }
  assert.equal(report.pageErrors.length, 0, 'No unexpected browser exceptions');
  assert.equal(report.failedRequests.length, 0, 'All required assets loaded');
  report.status = 'passed';
} catch (error) {report.error = String(error.stack || error); process.exitCode = 1;}
finally {
  clearInterval(receiptTimer);
  if (page && !page.isClosed()) {try {report.finalEvents = await invoke('events');
    if (report.status !== 'passed') report.observation = await invoke('observe');} catch {}}
  if (context) await context.close();
  save();
}
