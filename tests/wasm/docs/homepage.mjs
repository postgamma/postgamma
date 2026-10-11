/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
import {createRequire} from 'node:module';
import {writeFileSync, mkdirSync} from 'node:fs';
import {resolve, dirname} from 'node:path';
import {parseArgs} from 'node:util';
import assert from 'node:assert/strict';

const {values} = parseArgs({options: {
  root: {type: 'string'}, url: {type: 'string'}, output: {type: 'string'},
  engine: {type: 'string', default: 'chromium'}, executable: {type: 'string'},
  'static-host': {type: 'boolean'}, 'timeout-ms': {type: 'string', default: '45000'},
}});
const timeoutMs = Number(values['timeout-ms']);
assert(Number.isSafeInteger(timeoutMs) && timeoutMs > 0, 'timeout-ms must be a positive integer');
const require = createRequire(resolve(values.root, 'wasm/package.json'));
const playwright = require('playwright');
const report = {status: 'failed', url: values.url, timeoutMs, engine: values.engine, staticHost: !!values['static-host'], cases: [], errors: []};
mkdirSync(dirname(values.output), {recursive: true});
let browser, page;
const pendingRequests = new Set();
try {
  browser = await playwright[values.engine].launch({headless: true, timeout: 20000,
    ...(values.executable ? {executablePath: values.executable} : values.engine === 'chromium' ? {channel: 'chromium'} : {})});
  report.browserVersion = browser.version();
  const context = await browser.newContext({viewport: {width: 1440, height: 1100}});
  page = await context.newPage();
  page.setDefaultTimeout(timeoutMs);
  page.on('pageerror', error => report.errors.push(String(error)));
  page.on('request', request => pendingRequests.add(request));
  page.on('requestfinished', request => pendingRequests.delete(request));
  page.on('requestfailed', request => pendingRequests.delete(request));
  // Local smoke tests exclude external fonts / GitHub API. A public HTTPS
  // check uses the real network without intercepting the site's Worker fetches.
  if (new URL(values.url).protocol === 'http:')
    await context.route('https://**/*', route => route.abort());
  const assets = [];
  let navigations = 0;
  page.on('request', request => {if (/postgamma\.(wasm|data)/.test(request.url())) assets.push(request.url());});
  page.on('framenavigated', frame => {if (frame === page.mainFrame()) navigations++;});
  const status = page.locator('[data-pg-live-status]');
  const output = page.locator('[data-pg-live-results]');
  const run = page.locator('[data-pg-live-run]');
  async function completed() {
    await page.waitForFunction(() => ['Query complete', 'Output limit reached', 'Could not complete', 'Browser setup unavailable'].includes(document.querySelector('[data-pg-live-status]')?.textContent));
    assert.equal(await status.textContent(), 'Query complete', await output.textContent());
  }
  async function sql(query) {
    await page.locator('[data-pg-live-sql]').fill(query);
    await run.click();
    await completed();
    return output.textContent();
  }
  async function preset(name) {
    await page.locator('[data-pg-live-example]').selectOption(name);
    await run.click(); await completed();
    return output.textContent();
  }
  await page.goto(values.url, {waitUntil: 'domcontentloaded'});
  assert.equal(await status.textContent(), 'Not started');
  assert.equal(assets.length, 0);
  assert.equal(await page.evaluate(() => navigator.serviceWorker.getRegistrations().then(r => r.length)), 0);
  report.cases.push('no-startup-download-or-service-worker');
  await page.screenshot({path: values.output.replace(/\.json$/, '-before.png'), fullPage: true});
  await page.locator('[data-pg-live-sql]').fill("SELECT 'edited before start' AS proof;");
  await run.click(); await completed();
  assert.match(await output.textContent(), /edited before start/);
  assert.equal(await page.evaluate(() => isSecureContext), true);
  assert.equal(await page.evaluate(() => crossOriginIsolated), true);
  assert.equal(navigations, values['static-host'] ? 2 : 1);
  report.cases.push(values['static-host'] ? 'static-host-one-reload-preserves-sql' : 'native-headers-no-reload');
  assert.match(await preset('sql'), /Tools.*138\.00/s);
  assert.equal(await output.locator('table').count(), 1, 'one table per SELECT, without a duplicate terminal table');
  assert.match(await preset('json'), /Ada.*Sam.*Jo/s);
  assert.match(await preset('transaction'), /before rollback.*39\.00.*after rollback.*29\.00/s);
  assert.match(await preset('vector'), /PostgreSQL in the browser.*0\.000.*Local vector search.*0\.283/s);
  report.cases.push('sql-jsonb-rollback-pgvector');
  await page.locator('[data-pg-live-expand]').click();
  assert.equal(await page.locator('dialog[open]').count(), 1);
  await page.keyboard.press('Escape');
  await page.waitForSelector('dialog', {state: 'detached'});
  assert.equal(await page.locator('dialog').count(), 0);
  assert.equal(await run.textContent(), 'Run SQL');
  await page.locator('#pg-tab-python').click();
  assert.equal(await page.locator('#pg-panel-python').isVisible(), true);
  await page.locator('#pg-tab-static').click();
  assert.equal(await page.locator('#pg-panel-static').isVisible(), true);
  await page.locator('#pg-tab-browser').click();
  report.cases.push('expand-collapse-and-sdk-tabs');
  const literal = await sql("SELECT '<img src=x onerror=alert(1)>' AS literal, 9223372036854775807::bigint AS exact_integer, NULL AS absent;");
  assert.match(literal, /<img src=x onerror=alert\(1\)>/);
  assert.match(literal, /9223372036854775807/);
  assert.match(literal, /NULL/);
  assert.equal(await output.locator('img').count(), 0);
  report.cases.push('literal-rendering-int64-null');
  await sql('SELECT generate_series(1, 150) AS n;');
  assert.equal(await output.locator('table').count(), 1);
  assert.equal(await output.locator('tbody tr').count(), 150);
  await sql('SELECT 1 AS n; SELECT 2 AS n WHERE false; SELECT 3 AS n;');
  assert.equal(await output.locator('table').count(), 3);
  assert.equal(await output.locator('table').nth(1).locator('tbody tr').count(), 0);
  report.cases.push('chunked-and-empty-result-boundaries');
  await page.locator('[data-pg-live-sql]').fill('SELECT 1 / 0;'); await run.click();
  await page.waitForFunction(() => document.querySelector('[data-pg-live-status]').textContent === 'Could not complete');
  assert.match(await output.textContent(), /22012/);
  assert.match(await sql('SELECT 42 AS recovered;'), /42/);
  report.cases.push('sqlstate-and-error-recovery');
  await page.locator('[data-pg-live-sql]').fill('SELECT pg_sleep(20);'); await run.click();
  await page.waitForFunction(() => document.querySelector('[data-pg-live-status]').textContent === 'Running SQL…');
  await page.locator('[data-pg-live-cancel]').click();
  await page.waitForFunction(() => document.querySelector('[data-pg-live-status]').textContent === 'Canceled' && !document.querySelector('[data-pg-live-run]').disabled);
  assert.match(await sql('SELECT 43 AS after_cancel;'), /43/);
  report.cases.push('cancel-and-reuse');
  await page.locator('[data-pg-live-sql]').fill('SELECT generate_series(1, 500);'); await run.click();
  await page.waitForFunction(() => document.querySelector('[data-pg-live-status]').textContent === 'Output limit reached');
  assert.equal(await output.locator('tbody tr').count(), 200);
  assert.match(await sql('SELECT 44 AS after_limit;'), /44/);
  report.cases.push('bounded-output-and-reuse');
  await sql('UPDATE demo_orders SET total = 999 WHERE id = 1; SELECT total FROM demo_orders WHERE id = 1;');
  await page.locator('[data-pg-live-sql]').fill('SELECT total FROM demo_orders WHERE id = 1;');
  await page.locator('[data-pg-live-reset]').click(); await completed();
  assert.match(await output.textContent(), /29\.00/);
  report.cases.push('reset-discards-edits');
  await preset('sql');
  await page.screenshot({path: values.output.replace(/\.json$/, '-desktop.png'), fullPage: true});
  await page.setViewportSize({width: 390, height: 844});
  assert(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth + 1), 'mobile horizontal overflow');
  await page.locator('#live-demo').scrollIntoViewIfNeeded();
  await page.screenshot({path: values.output.replace(/\.json$/, '-mobile.png')});
  await page.locator('[data-pg-theme-toggle]').click();
  await page.screenshot({path: values.output.replace(/\.json$/, '-light.png')});
  report.cases.push('mobile-layout-and-theme');
  await page.locator('[data-pg-live-close]').click();
  await page.waitForFunction(() => document.querySelector('[data-pg-live-status]').textContent === 'Database closed');
  const count = assets.length;
  await page.reload({waitUntil: 'domcontentloaded'});
  assert.equal(await status.textContent(), 'Not started');
  assert.equal(assets.length, count, 'reload must not start the database');
  report.cases.push('close-and-no-autostart-on-revisit');
  if (!values['static-host']) {
    // Cancel during a real, delayed startup download, then retry the same UI.
    await page.route('**/postgamma.data', async route => {
      await new Promise(resolve => setTimeout(resolve, 600));
      await route.continue().catch(() => {});
    });
    await run.click();
    await page.waitForFunction(() => document.querySelector('[data-pg-live-status]').textContent === 'Loading PostgreSQL…');
    await page.locator('[data-pg-live-cancel]').click();
    await page.waitForFunction(() => document.querySelector('[data-pg-live-status]').textContent === 'Canceled' && !document.querySelector('[data-pg-live-run]').disabled);
    await page.unroute('**/postgamma.data');
    assert.match(await sql('SELECT 45 AS after_startup_cancel;'), /45/);
    await page.locator('[data-pg-live-close]').click();
    await page.waitForFunction(() => document.querySelector('[data-pg-live-status]').textContent === 'Database closed');
    report.cases.push('cancel-startup-and-retry');
  }
  await page.goto(new URL('getting-started/browser/', values.url).href, {waitUntil: 'domcontentloaded'});
  await page.locator('.md-header__button[for="__search"]').click();
  await page.locator('.md-search__input').fill('Browser');
  await page.waitForFunction(() => document.querySelector('.md-search-result__list')?.textContent.includes('Browser preview'));
  report.cases.push('documentation-search-after-demo');
  assert.deepEqual(report.errors, []);
  report.status = 'passed';
} catch (error) {
  report.error = error.stack; process.exitCode = 1;
  report.pendingRequests = [...pendingRequests].map(request => request.url());
  if (page && !page.isClosed()) {
    try {
      report.ui = await page.evaluate(() => ({
        status: document.querySelector('[data-pg-live-status]')?.textContent,
        detail: document.querySelector('[data-pg-live-detail]')?.textContent,
        output: document.querySelector('[data-pg-live-results]')?.textContent,
        secure: isSecureContext, isolated: crossOriginIsolated,
      }));
      await page.screenshot({path: values.output.replace(/\.json$/, '-failure.png')});
    } catch (_) { /* Preserve the original failure if the page has already gone. */ }
  }
}
finally {await browser?.close(); writeFileSync(values.output, JSON.stringify(report, null, 2) + '\n');}
console.log(JSON.stringify(report));
