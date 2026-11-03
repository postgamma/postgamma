/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
import {createRequire} from 'node:module';
import {writeFileSync, mkdirSync, mkdtempSync, rmSync} from 'node:fs';
import {resolve, dirname, join} from 'node:path';
import {tmpdir} from 'node:os';
import {parseArgs} from 'node:util';
import assert from 'node:assert/strict';

const {values} = parseArgs({options: {root: {type: 'string'}, url: {type: 'string'},
  output: {type: 'string'}, engine: {type: 'string', default: 'chromium'},
  executable: {type: 'string'}, 'static-host': {type: 'boolean'}}});
const require = createRequire(resolve(values.root, 'wasm/package.json'));
const playwright = require('playwright');
const report = {status: 'failed', engine: values.engine, staticHost: !!values['static-host'],
  cases: [], errors: [], consoleErrors: [], requestsFailed: []};
const profile = mkdtempSync(join(tmpdir(), 'postgamma-docs-opfs-'));
const url = new URL('opfs/', values.url).href;
const receiptKey = 'postgamma:opfs-receipt:' + new URL(values.url).pathname;
mkdirSync(dirname(values.output), {recursive: true});
let context, page, downloads = 0, navigations = 0;
const started = Date.now();
const passed = name => {report.cases.push(name); console.log('passed: ' + name + ' (' + (Date.now() - started) + 'ms)');};
const uiReport = () => page.locator('[data-opfs-report]').textContent().then(JSON.parse);
async function launch() {
  context = await playwright[values.engine].launchPersistentContext(profile, {
    headless: true, timeout: 20000, viewport: {width: 1280, height: 960},
    ...(values.executable ? {executablePath: values.executable} : values.engine === 'chromium' ? {channel: 'chromium'} : {}),
  });
  page = context.pages()[0] || await context.newPage();
  page.setDefaultTimeout(60000);
  page.on('pageerror', error => report.errors.push(String(error)));
  page.on('console', message => {
    if (message.type() === 'error' && report.consoleErrors.length < 40) report.consoleErrors.push(message.text());
  });
  page.on('requestfailed', request => {
    if (request.url().startsWith(values.url) && report.requestsFailed.length < 40)
      report.requestsFailed.push({url: request.url(), error: request.failure()?.errorText});
  });
  page.on('framenavigated', frame => {if (frame === page.mainFrame()) navigations++;});
  page.on('request', request => {if (/postgamma\.(wasm|data)/.test(request.url())) downloads++;});
  if (new URL(values.url).protocol === 'http:') await context.route('https://**/*', route => route.abort());
  await page.goto(url, {waitUntil: 'domcontentloaded'});
}
async function completed(expected = 'Saved data verified') {
  await page.waitForFunction(() => {
    const button = document.querySelector('[data-opfs-write]');
    const status = document.querySelector('[data-opfs-status]');
    return button && !button.disabled &&
      ['Saved data verified', 'Recovery verified', 'Check failed', 'Test cancelled'].includes(status?.textContent);
  }, undefined, {timeout: 130000}); // Let the page's own 120-second deadline report failures.
  assert.equal(await page.locator('[data-opfs-status]').textContent(), expected, JSON.stringify(await uiReport()));
  return uiReport();
}
async function closeDatabase() {
  await page.locator('details').evaluate(el => {el.open = true;});
  await page.locator('[data-opfs-close]').click();
  await page.waitForFunction(() => document.querySelector('[data-opfs-status]').textContent === 'Database closed' &&
    !document.querySelector('[data-opfs-write]').disabled);
}
try {
  await launch();
  assert.equal(downloads, 0, 'visiting the test page does not start PostgreSQL');
  assert(await page.locator('[data-opfs-verify]').isDisabled());
  const note = "Safari OPFS — 中文 👋 <img src=x onerror=alert(1)>";
  await page.locator('[data-opfs-note]').fill(note);
  await page.locator('[data-opfs-write]').click();
  const first = await completed();
  assert.equal(first.environment.isolated, true);
  assert.equal(first.checks.write, 'passed');
  assert.equal(first.checks.reopen, 'passed');
  assert.equal(first.actual.note, note);
  assert.equal(first.actual.marker, first.expected.marker);
  assert.equal(await page.locator('.pg-opfs img').count(), 0);
  assert.equal(navigations, values['static-host'] ? 2 : 1);
  const receipt = await page.evaluate(key => localStorage.getItem(key), receiptKey);
  report.first = first;
  passed('real-opfs-write-fresh-instance-reopen-unicode-and-literal-rendering');
  if (values['static-host']) passed('project-pages-one-reload-preserves-test-message');
  await page.locator('[data-pg-opfs]').screenshot({path: values.output.replace(/\.json$/, '-desktop.png')});
  await page.setViewportSize({width: 390, height: 844});
  assert(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth + 1));
  await page.locator('[data-pg-opfs]').screenshot({path: values.output.replace(/\.json$/, '-mobile.png')});
  await page.setViewportSize({width: 320, height: 740});
  assert(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth + 1));
  passed('390-and-320-pixel-layout');

  // Real browser process restart, preserving its disk profile. Do not call
  // database.close(): the last acknowledged marker must still survive.
  await context.close(); context = undefined;
  const priorDownloads = downloads;
  await launch();
  assert.equal(downloads, priorDownloads, 'revisiting does not create a new database');
  assert.equal(await page.locator('[data-opfs-status]').textContent(), 'Saved test found');
  assert.equal(await page.locator('[data-opfs-actual]').textContent(), 'Not read in this page visit');
  await page.locator('[data-opfs-verify]').click();
  const reopened = await completed();
  assert.deepEqual(reopened.actual, first.actual);
  passed('committed-record-survives-browser-process-restart');

  // A mismatching receipt must fail rather than replace/seed the database.
  await page.evaluate(({key, receipt}) => {
    const value = JSON.parse(receipt); value.marker = crypto.randomUUID();
    localStorage.setItem(key, JSON.stringify(value));
  }, {key: receiptKey, receipt});
  await page.locator('[data-opfs-verify]').click();
  const mismatch = await completed('Check failed');
  assert.match(mismatch.error.message, /does not match/);
  assert.equal(mismatch.actual.marker, first.actual.marker);
  await page.evaluate(({key, receipt}) => localStorage.setItem(key, receipt), {key: receiptKey, receipt});
  await page.locator('[data-opfs-verify]').click(); await completed();
  passed('mismatch-fails-without-overwriting-postgresql-data');

  await page.locator('details').evaluate(el => {el.open = true;});
  const navigation = page.waitForEvent('framenavigated', frame => frame === page.mainFrame());
  await page.locator('[data-opfs-interrupt]').click();
  await navigation;
  const recovered = await completed('Recovery verified');
  assert.equal(recovered.checks.interruptedTransaction, 'passed');
  assert.deepEqual(recovered.actual, first.actual);
  passed('reload-with-uncommitted-update-recovers-original-record');
  report.recovered = recovered;

  await closeDatabase();
  // Remove only this harness's database inside its new, private browser profile.
  // The retained receipt must not cause Verify to silently initialize an empty DB.
  await page.evaluate(async name => {
    const root = await (await navigator.storage.getDirectory()).getDirectoryHandle('postgamma');
    await root.removeEntry(name, {recursive: true});
  }, first.database);
  await page.locator('[data-opfs-verify]').click();
  const absent = await completed('Check failed');
  assert.equal(absent.actual, null);
  passed('missing-database-fails-closed-without-reseeding');
  await page.locator('[data-opfs-write]').click();
  const replacement = await completed();
  assert.notEqual(replacement.actual.marker, first.actual.marker);
  await closeDatabase();
  passed('explicit-write-can-start-a-new-test-after-data-deletion');
  assert.deepEqual(report.errors, []);
  report.status = 'passed';
} catch (error) {
  report.error = error.stack; process.exitCode = 1;
  if (page && !page.isClosed()) {
    try {
      report.ui = await uiReport();
      report.statusText = await page.locator('[data-opfs-status]').textContent();
      await page.screenshot({path: values.output.replace(/\.json$/, '-failure.png')});
    } catch (_) {}
  }
} finally {
  await context?.close();
  rmSync(profile, {recursive: true, force: true});
  writeFileSync(values.output, JSON.stringify(report, null, 2) + '\n');
}
console.log(JSON.stringify(report));
