/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
import {createRequire} from 'node:module';
import {readFileSync, writeFileSync, renameSync} from 'node:fs';
import {resolve} from 'node:path';
import {parseArgs} from 'node:util';
import {failureSnapshot} from '../browser/diagnostics.mjs';

const {values} = parseArgs({options: {
  root: {type: 'string'}, url: {type: 'string'}, output: {type: 'string'}, profile: {type: 'string'},
  cases: {type: 'string'}, engine: {type: 'string'}, executable: {type: 'string'},
  'start-index': {type: 'string', default: '0'},
}});
if (!['chromium', 'firefox', 'webkit'].includes(values.engine)) throw new Error('Unknown engine');
const require = createRequire(resolve(values.root, 'wasm/package.json'));
const playwright = require('playwright');
const allIds = JSON.parse(readFileSync(values.cases, 'utf8')).cases.map(c => c.id);
const startIndex = Number(values['start-index']);
if (!Number.isSafeInteger(startIndex) || startIndex < 0 || startIndex >= allIds.length) throw new Error('Invalid case start index');
const ids = allIds.slice(startIndex);
const report = {status: 'failed', engine: values.engine, playwright: require('playwright/package.json').version,
  expectedCases: ids, startupTimeoutMs: 180000, caseTimeoutMs: 90000, cases: [], events: [], console: [], pageErrors: [], failedRequests: []};
const errorText = error => [String(error), error?.stack].filter(Boolean).join('\n');
const save = () => {
  writeFileSync(values.output + '.tmp', JSON.stringify(report, null, 2) + '\n');
  renameSync(values.output + '.tmp', values.output);
};
let context, page;
save();
try {
  context = await playwright[values.engine].launchPersistentContext(values.profile, {
    headless: true, timeout: 15000,
    ...(values.executable ? {executablePath: values.executable} : values.engine === 'chromium' ? {channel: 'chromium'} : {}),
  });
  report.browserVersion = context.browser().version();
  page = context.pages()[0] || await context.newPage();
  page.on('console', message => {report.console.push({type: message.type(), text: message.text()});
    if (report.console.length > 1000) report.console.shift();});
  page.on('pageerror', error => {report.pageErrors.push(errorText(error)); save();});
  page.on('requestfailed', request => {report.failedRequests.push({url: request.url(), error: request.failure()?.errorText}); save();});
  page.on('response', response => {if (response.status() >= 400) {report.failedRequests.push({url: response.url(), status: response.status()}); save();}});
  await page.exposeFunction('pgliteEvent', event => {
    report.events.push(event);
    if (event.kind === 'ready') {
      if (JSON.stringify(event.ids) !== JSON.stringify(ids)) throw new Error('Browser test inventory differs from source');
      report.ready = true; report.storage = event.storage;
    }
    if (event.kind === 'case-end') {
      report.cases.push(event.result);
      console.log(`${values.engine}/${report.storage} ${event.result.id}: ${event.result.status} (${Math.round(event.result.elapsedMs)} ms)`);
    }
    if (event.kind === 'done') report.probe = event.result;
    save();
  });
  await page.goto(values.url, {timeout: 15000});
  await page.waitForFunction(() => window.probeResult !== null ||
    document.documentElement.dataset.pgliteReady === 'true', undefined, {timeout: report.startupTimeoutMs});
  for (let count = 1; count <= ids.length; count++) {
    await page.waitForFunction(n => window.probeResult !== null ||
      Number(document.documentElement.dataset.pgliteCompleted || 0) >= n,
      count, {timeout: report.caseTimeoutMs});
    if (await page.evaluate(() => window.probeResult !== null)) break;
  }
  await page.waitForFunction(() => window.probeResult !== null, undefined, {timeout: 90000});
  report.probe = await page.evaluate(() => window.probeResult);
  if (report.probe.status !== 'passed' || !report.probe.isolated || !report.probe.worker || !report.probe.sharedMemory)
    throw new Error(report.probe.error || 'Missing real isolated Wasm execution');
  if (JSON.stringify(report.cases.map(c => c.id)) !== JSON.stringify(ids) || report.cases.some(c => c.status !== 'passed'))
    throw new Error('Incomplete or failed PGlite-derived cases');
  if (report.pageErrors.length || report.failedRequests.length) throw new Error('Browser page or asset error');
  report.status = 'passed';
} catch (error) {
  report.error = errorText(error); process.exitCode = 1;
  if (page && !page.isClosed()) {
    report.controlFailure = await failureSnapshot(page);
    let timer;
    try {report.failureSnapshot = await Promise.race([
      page.evaluate(() => window.pgliteSnapshot?.()),
      new Promise((_, reject) => {timer = setTimeout(() => reject(new Error('snapshot timeout')), 5000);}),
    ]);} catch (snapshotError) {report.snapshotError = String(snapshotError);}
    finally {clearTimeout(timer);}
  }
} finally {
  report.unfinishedCases = ids.filter(id => !report.cases.some(c => c.id === id));
  // Persist evidence before closing a possibly unhealthy browser context.
  save();
  try {if (context) await context.close();}
  catch (error) {report.status = 'failed'; report.closeError = String(error); process.exitCode = 1;}
  save();
}
