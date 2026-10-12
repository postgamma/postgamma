/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
import {createRequire} from 'node:module';
import {writeFileSync, mkdirSync} from 'node:fs';
import {resolve, dirname} from 'node:path';
import {parseArgs} from 'node:util';
import assert from 'node:assert/strict';
const {values} = parseArgs({options: {root: {type: 'string'}, url: {type: 'string'},
  output: {type: 'string'}, engine: {type: 'string', default: 'chromium'},
  executable: {type: 'string'}, 'static-host': {type: 'boolean'}}});
const require = createRequire(resolve(values.root, 'wasm/package.json'));
const playwright = require('playwright');
const report = {status: 'failed', engine: values.engine, staticHost: !!values['static-host'], cases: [], errors: [], requestsFailed: [], webSockets: []};
function passed(...cases) {report.cases.push(...cases); console.log('passed: ' + cases.join(', '));}
mkdirSync(dirname(values.output), {recursive: true});
let browser, page;
try {
  browser = await playwright[values.engine].launch({headless: true, timeout: 20000,
    ...(values.executable ? {executablePath: values.executable} : values.engine === 'chromium' ? {channel: 'chromium'} : {})});
  report.browserVersion = browser.version();
  const context = await browser.newContext({viewport: {width: 1440, height: 1000}});
  page = await context.newPage(); page.setDefaultTimeout(45000);
  page.on('pageerror', error => report.errors.push(String(error)));
  page.on('websocket', socket => report.webSockets.push(socket.url()));
  page.on('requestfailed', request => {if (request.url().startsWith(values.url)) report.requestsFailed.push(request.url());});
  if (new URL(values.url).protocol === 'http:') await context.route('https://**/*', route => route.abort());
  let navigations = 0, downloads = 0;
  page.on('framenavigated', frame => {if (frame === page.mainFrame()) navigations++;});
  page.on('request', request => {if (/postgamma\.(wasm|data)/.test(request.url())) downloads++;});
  await context.addInitScript(() => {
    window.psqlText = () => {
      const term = document.querySelector('[data-pg-psql-terminal]')?.terminal;
      if (!term) return '';
      const buffer = term.buffer.active, end = buffer.baseY + buffer.cursorY;
      let text = '';
      for (let row = 0; row <= end; row++) {
        if (row && !buffer.getLine(row).isWrapped) text += '\n';
        text += buffer.getLine(row).translateToString(row !== end, 0, row === end ? buffer.cursorX : undefined);
      }
      return text;
    };
  });
  const screen = page.locator('[data-pg-psql-terminal]'), input = screen.locator('textarea');
  const read = () => page.evaluate(() => window.psqlText());
  async function paste(text) {
    await input.focus();
    await input.evaluate((element, text) => {
      // Firefox empties DataTransfer on synthetic ClipboardEvent dispatch.
      // Supply the clipboard interface directly and exercise xterm's real handler.
      const event = new Event('paste', {bubbles: true, cancelable: true});
      Object.defineProperty(event, 'clipboardData', {value: {getData: type => type === 'text/plain' ? text : ''}});
      element.dispatchEvent(event);
    }, text);
  }
  async function ready(prompt = 'postgres=# ') {
    await page.waitForFunction(prompt => document.querySelector('[data-pg-psql-status]')?.textContent === 'Ready' &&
      window.psqlText().endsWith(prompt), prompt);
  }
  async function command(text, expected, prompt = 'postgres=# ') {
    const before = await read();
    await paste(text); await input.press('Enter');
    await page.waitForFunction(({before, expected, prompt}) => window.psqlText() !== before &&
      window.psqlText().endsWith(prompt) && new RegExp(expected, 's').test(window.psqlText()),
      {before, expected: expected.source, prompt});
    await ready(prompt);
    return await read();
  }
  let releaseStartup;
  if (!values['static-host']) {
    const gate = new Promise(resolve => {releaseStartup = resolve;});
    await context.route('**/assets/wasm/demo.json', async route => {await gate; await route.continue();});
  }
  await page.goto(values.url, {waitUntil: 'domcontentloaded'});
  if (releaseStartup) {
    const loading = page.locator('[data-pg-psql-loading]');
    await page.waitForFunction(() => document.querySelector('[data-pg-psql-loading-title]').textContent === 'Loading PostgreSQL');
    assert(await loading.isVisible());
    assert(await screen.evaluate(el => el.inert));
    assert((await screen.boundingBox()).height <= 370, 'loading keeps the compact terminal height');
    assert.equal(await page.locator('[data-pg-psql-loading-example]').count(), 0);
    const progress = page.locator('[data-pg-psql-progress]');
    const firstProgress = Number(await progress.getAttribute('aria-valuenow'));
    await page.waitForFunction(first => Number(document.querySelector('[data-pg-psql-progress]').getAttribute('aria-valuenow')) > first + 3, firstProgress);
    const {percent, label} = await progress.evaluate(el => ({percent: Number(el.getAttribute('aria-valuenow')),
      label: document.querySelector('[data-pg-psql-loading-percent]').textContent}));
    assert(percent < 95, 'an incomplete startup must not report completion');
    assert.equal(label, '~' + percent + '%');
    const frame = await loading.boundingBox(), number = await page.locator('[data-pg-psql-loading-percent]').boundingBox();
    assert(Math.abs(number.x + number.width / 2 - frame.x - frame.width / 2) < 2, 'loading percentage is centered');
    assert.match(await page.locator('[data-pg-psql-loading-time]').textContent(), /[1-9]\d*s/);
    await page.locator('#live-demo').screenshot({path: values.output.replace(/\.json$/, '-loading.png')});
    await page.setViewportSize({width: 390, height: 844});
    await page.locator('#live-demo').scrollIntoViewIfNeeded();
    assert(await loading.evaluate(el => el.scrollHeight <= el.clientHeight + 1), 'mobile loading content fits');
    assert(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth + 1));
    await page.locator('#live-demo').screenshot({path: values.output.replace(/\.json$/, '-loading-mobile.png')});
    await page.setViewportSize({width: 1440, height: 1000});
    releaseStartup(); await context.unroute('**/assets/wasm/demo.json');
    passed('centered-estimated-progress-elapsed-no-tips-and-compact-mobile-layout');
  }
  await ready();
  assert(downloads >= 2, 'Wasm and database assets download automatically');
  assert.equal(await page.locator('[data-pg-psql-start], [data-pg-psql-send]').count(), 0);
  assert.equal(await input.evaluate(el => getComputedStyle(el).opacity), '0');
  assert.match(await read(), /psql \(/);
  assert.doesNotMatch(await read(), /could not find a .*psql.* to execute/);
  assert(await page.locator('[data-pg-psql-loading]').isHidden());
  assert((await screen.boundingBox()).height <= 370, 'compact terminal height');
  await page.locator('#live-demo').screenshot({path: values.output.replace(/\.json$/, '-ready.png')});
  report.isolated = await page.evaluate(() => isSecureContext && crossOriginIsolated && typeof SharedArrayBuffer === 'function');
  assert.equal(report.isolated, true);
  assert.equal(navigations, values['static-host'] ? 2 : 1);
  passed('automatic-real-psql-startup-with-inline-terminal', values['static-host'] ? 'pages-project-prefix-service-worker-one-reload' : 'native-headers-no-reload');
  // Drive the terminal's real keyboard and clipboard events, with no separate
  // command input or submit button and no direct calls into the database SDK.
  assert.equal(await page.locator('dialog[open]').count(), 0, 'startup does not expand');
  await input.focus(); await input.press('ArrowLeft'); await input.press('Backspace');
  assert.equal(await page.locator('dialog[open]').count(), 0, 'control keys do not expand');
  await page.keyboard.type('S');
  await page.waitForSelector('dialog[open]');
  await page.waitForFunction(() => window.psqlText().endsWith('postgres=# S'));
  assert(await input.evaluate(el => el === document.activeElement), 'expansion retains keyboard focus');
  await page.screenshot({path: values.output.replace(/\.json$/, '-first-input.png')});
  await page.keyboard.type('ELECT 123;');
  await page.waitForFunction(() => window.psqlText().endsWith('postgres=# SELECT 123;'));
  await input.press('Escape'); await page.waitForSelector('dialog', {state: 'detached'});
  await input.press('ArrowLeft'); await input.press('Backspace');
  await page.keyboard.type('4'); await input.press('End'); await input.press('Enter');
  await page.waitForFunction(() => /\n\s*124\s*\n/.test(window.psqlText())); await ready();
  assert.equal(await page.locator('dialog[open]').count(), 0, 'typing after collapse stays compact');
  passed('first-character-expands-once-without-losing-input-or-focus');
  await input.press('ArrowUp'); await input.press('Home'); await input.press('Delete');
  await page.keyboard.type('S'); await input.press('Control+e'); await input.press('Enter'); await ready();
  await page.waitForFunction(() => window.psqlText().endsWith('(1 row)\n\npostgres=# '));
  await page.keyboard.type('draft'); await input.press('ArrowUp'); await input.press('ArrowDown');
  await page.waitForFunction(() => window.psqlText().endsWith('postgres=# draft'));
  await input.press('Control+u');
  await page.keyboard.insertText("SELECT '中文e\u0301🙂"); await input.press('Backspace'); await input.press('Backspace');
  await page.keyboard.insertText("' AS edited;"); await input.press('Enter');
  await page.waitForFunction(() => /edited\s*\n[-]+\s*\n 中文/.test(window.psqlText())); await ready();
  await input.press('Control+l'); await ready();
  await paste("SELECT length('" + 'a'.repeat(3000) + "') AS long_input;");
  await input.press('Home'); await input.press('Delete'); await page.keyboard.type('S');
  await input.press('End'); await input.press('Enter');
  await page.waitForFunction(() => /\n\s*3000\s*\n/.test(window.psqlText())); await ready();
  assert.equal((await read()).split('postgres=# SELECT length(').length - 1, 1, 'long input echoes once');
  passed('inline-echo-cursor-edit-delete-home-end-history-draft-unicode-grapheme-ctrl-l');
  await command('\\?', /General.*\\copyright.*quit psql/);
  await command('CREATE DATABASE terminal_test;', /CREATE DATABASE/);
  await command('\\c terminal_test', /You are now connected to database "terminal_test"/, 'terminal_test=# ');
  await command('CREATE TABLE t1(c1 integer, c2 text);', /CREATE TABLE/, 'terminal_test=# ');
  await command("INSERT INTO t1 VALUES (100, '你好');", /INSERT 0 1/, 'terminal_test=# ');
  const rows = await command('SELECT * FROM t1;', /100\s*\| 你好/, 'terminal_test=# ');
  assert.match(rows, /\(1 row\)/);
  await command('\\d t1', /Table "public.t1".*integer.*text/, 'terminal_test=# ');
  await command('\\l', /List of databases.*terminal_test/, 'terminal_test=# ');
  passed('help-create-database-connect-ddl-insert-select-describe-list-unicode');
  await command('SELECT * FROM missing_table;', /ERROR:.*does not exist/, 'terminal_test=# ');
  await command('\\c missing_database', /does not exist.*Previous connection kept/, 'terminal_test=# ');
  await command('SELECT 42 AS recovered;', /\n\s*42\s*\n/, 'terminal_test=# ');
  await command('BEGIN; INSERT INTO t1 VALUES(200, \'rolled back\'); ROLLBACK;', /BEGIN.*INSERT 0 1.*ROLLBACK/, 'terminal_test=# ');
  await command('SELECT count(*) FROM t1;', /\n\s*1\s*\n/, 'terminal_test=# ');
  passed('sql-error-failed-reconnect-keeps-session-rollback');
  await command('SELECT', /terminal_test-# /, 'terminal_test-# ');
  await command('73 AS multiline;', /\n\s*73\s*\n/, 'terminal_test=# ');
  await command('SELECT', /terminal_test-# /, 'terminal_test-# ');
  await input.press('Control+c'); await ready('terminal_test=# ');
  await command('SELECT 74 AS after_input_cancel;', /\n\s*74\s*\n/, 'terminal_test=# ');
  await paste('SELECT pg_sleep(30);'); await input.press('Enter');
  // A query-side acknowledgement prevents cancellation from racing submission.
  await page.waitForFunction(() => document.querySelector('[data-pg-psql-status]').textContent === 'Running…');
  await page.waitForTimeout(300);
  await page.locator('[data-pg-psql-cancel]').click();
  await page.waitForFunction(() => window.psqlText().includes('canceling statement'));
  await ready('terminal_test=# ');
  await command('SELECT 75 AS after_query_cancel;', /\n\s*75\s*\n/, 'terminal_test=# ');
  passed('multiline-input-cancel-query-cancel-and-reuse');
  await command('COPY t1 FROM STDIN;\n300\tcopy-input\n\\.', /COPY 1/, 'terminal_test=# ');
  await command('COPY (SELECT c1 FROM t1 ORDER BY c1) TO STDOUT;', /\n100\n300\n/, 'terminal_test=# ');
  await command("SELECT '<img src=x onerror=alert(1)>' AS literal;", /\(1 row\)/, 'terminal_test=# ');
  assert.equal(await screen.locator('img').count(), 0);
  await command('SELECT generate_series(1, 30000);', /\(30000 rows\)/, 'terminal_test=# ');
  assert(await screen.evaluate(el => el.terminal.buffer.active.length <= 2000 + el.terminal.rows));
  await command('SELECT 76 AS after_output_backpressure;', /\n\s*76\s*\n/, 'terminal_test=# ');
  passed('copy-stdin-stdout-literal-rendering-bounded-output-backpressure');
  await command('\\c terminal_test postgamma example.com', /network connection strings are unavailable.*Previous connection kept/, 'terminal_test=# ');
  await command('\\watch', /could not set timer/, 'terminal_test=# ');
  await command('SELECT 77 AS after_unsupported_command;', /\n\s*77\s*\n/, 'terminal_test=# ');
  passed('unsupported-host-and-watch-recover-without-network-proxy');
  await page.locator('[data-pg-live-expand]').click(); assert.equal(await page.locator('dialog[open]').count(), 1);
  await page.keyboard.press('Escape'); await page.waitForSelector('dialog', {state: 'detached'});
  await page.screenshot({path: values.output.replace(/\.json$/, '-desktop.png'), fullPage: true});
  await page.setViewportSize({width: 390, height: 844});
  assert(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth + 1), 'mobile horizontal overflow');
  await page.locator('#live-demo').scrollIntoViewIfNeeded();
  const wrapped = "SELECT '" + 'long_'.repeat(14) + "' AS wrapped;";
  await paste(wrapped); await input.press('Home'); await input.press('Delete'); await page.keyboard.type('S');
  await input.press('End');
  await page.locator('[data-pg-live-expand]').click();
  await page.keyboard.press('Escape'); await page.waitForSelector('dialog', {state: 'detached'});
  await input.press('Enter');
  await page.waitForFunction(() => window.psqlText().includes('(1 row)') && window.psqlText().endsWith('terminal_test=# '));
  assert.match(await read(), /long_long_long_long_/); await ready('terminal_test=# ');
  await page.screenshot({path: values.output.replace(/\.json$/, '-mobile.png')});
  passed('expand-collapse-mobile-layout-wrapped-input-resize-and-edit');
  await paste('\\q'); await input.press('Enter');
  await page.waitForFunction(() => document.querySelector('[data-pg-psql-status]').textContent === 'psql exited (0)');
  await input.press('Enter'); await ready();
  await command("SELECT datname FROM pg_database WHERE datname = 'terminal_test';", /\(0 rows\)/);
  await page.locator('[data-pg-psql-reset]').click(); await ready();
  await paste('SELECT pg_sleep(30);'); await input.press('Enter');
  assert.equal(await page.locator('dialog[open]').count(), 0, 'database reset does not rearm automatic expansion');
  await page.waitForTimeout(300); await page.locator('[data-pg-psql-close]').click();
  await page.waitForFunction(() => document.querySelector('[data-pg-psql-status]').textContent === 'Database closed');
  const count = downloads; await page.reload({waitUntil: 'domcontentloaded'});
  await ready(); assert(downloads > count, 'reload starts a fresh session automatically');
  await command('SELECT 78 AS after_reload;', /\n\s*78\s*\n/);
  assert.equal(await page.locator('dialog[open]').count(), 1, 'first paste after page reload expands');
  await input.press('Escape'); await page.waitForSelector('dialog', {state: 'detached'});
  await input.press('Control+d');
  await page.waitForFunction(() => document.querySelector('[data-pg-psql-status]').textContent === 'Database closed');
  passed('quit-enter-to-restart-reset-close-active-query-reload-autostart-ctrl-d');
  // A controlled service worker owns requests in the Pages profile; inject
  // the unavailable-package response on the native-header host instead.
  if (!values['static-host']) {
    await context.route('**/assets/wasm/demo.json', route => route.fulfill({json: {available: false}}));
    await page.reload({waitUntil: 'domcontentloaded'});
    await page.waitForFunction(() => document.querySelector('[data-pg-psql-status]').textContent === 'Could not start');
    assert.match(await read(), /has not been bundled/);
    await context.unroute('**/assets/wasm/demo.json');
    await input.press('Enter'); await ready();
    assert.equal(await page.locator('dialog[open]').count(), 0, 'Enter to retry is not text input');
    await input.focus(); await page.keyboard.insertText('中');
    await page.waitForSelector('dialog[open]');
    await page.waitForFunction(() => window.psqlText().endsWith('postgres=# 中'));
    await input.press('Escape'); await page.waitForSelector('dialog', {state: 'detached'});
    await input.press('Control+u');
    await command('SELECT 79 AS after_startup_retry;', /\n\s*79\s*\n/);
    assert.equal(await page.locator('dialog[open]').count(), 0, 'input events expand only once');
    await page.locator('[data-pg-psql-close]').click();
    await page.waitForFunction(() => document.querySelector('[data-pg-psql-status]').textContent === 'Database closed');
    passed('automatic-startup-error-visible-and-enter-to-retry');
  }
  assert.deepEqual(report.errors, []);
  assert.deepEqual(report.requestsFailed, []);
  assert.deepEqual(report.webSockets, []);
  report.status = 'passed';
} catch (error) {
  report.error = error.stack; process.exitCode = 1;
  if (page && !page.isClosed()) {
    report.ui = await page.evaluate(() => ({status: document.querySelector('[data-pg-psql-status]')?.textContent,
      output: window.psqlText?.()})).catch(() => null);
    await page.screenshot({path: values.output.replace(/\.json$/, '-failure.png')}).catch(() => {});
  }
} finally {await browser?.close(); writeFileSync(values.output, JSON.stringify(report, null, 2) + '\n');}
console.log(JSON.stringify(report));
