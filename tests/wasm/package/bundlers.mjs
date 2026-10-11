/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
import assert from 'node:assert/strict';
import {createRequire} from 'node:module';
import {resolve} from 'node:path';
import {writeFile} from 'node:fs/promises';
import {parseArgs} from 'node:util';
const {values} = parseArgs({options: {root: {type: 'string'}, url: {type: 'string'},
  output: {type: 'string'}, executable: {type: 'string'}}});
const require = createRequire(resolve(values.root, 'wasm/package.json'));
const {chromium} = require('playwright');
const report = {status: 'failed', url: values.url, pageErrors: [], httpErrors: []};
let browser;
try {
  browser = await chromium.launch({headless: true, timeout: 15000,
    ...(values.executable ? {executablePath: values.executable} : {channel: 'chromium'})});
  report.browserVersion = browser.version();
  const page = await browser.newPage();
  page.on('pageerror', error => report.pageErrors.push(String(error)));
  page.on('response', response => {if (response.status() >= 400) report.httpErrors.push({url: response.url(), status: response.status()});});
  await page.goto(values.url, {timeout: 15000});
  await page.waitForFunction(() => window.postgammaExample !== undefined, undefined, {timeout: 120000});
  report.probe = await page.evaluate(() => window.postgammaExample);
  assert.equal(report.probe.status, 'passed', report.probe.error);
  assert.equal(report.probe.value, '9007199254740993');
  assert.deepEqual(report.pageErrors, []); assert.deepEqual(report.httpErrors, []);
  report.status = 'passed';
} catch (error) {report.error = String(error.stack || error); process.exitCode = 1;}
finally {await browser?.close(); await writeFile(values.output, JSON.stringify(report, null, 2) + '\n');}
