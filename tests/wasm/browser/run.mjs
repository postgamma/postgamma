/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
import {createRequire} from 'node:module';
import {readFileSync, writeFileSync} from 'node:fs';
import {dirname, resolve} from 'node:path';
import {parseArgs} from 'node:util';

const {values} = parseArgs({options: {
  root: {type: 'string'},
  url: {type: 'string'},
  output: {type: 'string'},
  describe: {type: 'boolean'},
  executable: {type: 'string'},
  profile: {type: 'string'},
}});
const require = createRequire(resolve(values.root, 'wasm/package.json'));
const {chromium} = require('playwright');
const report = {
  status: values.describe ? 'not-run' : 'failed',
  playwright: require('playwright/package.json').version,
  executable: values.executable || chromium.executablePath(),
  browserRevisions: JSON.parse(readFileSync(resolve(dirname(require.resolve('playwright-core/package.json')), 'browsers.json'))).browsers,
  console: [],
  pageErrors: [],
  failedRequests: [],
};
if (values.describe) {
  console.log(JSON.stringify(report));
} else {
  let context;
  try {
    context = await chromium.launchPersistentContext(values.profile, {
      headless: true,
      ...(values.executable ? {executablePath: values.executable} : {channel: 'chromium'}),
      timeout: 15000,
    });
    report.browserVersion = context.browser().version();
    report.profile = values.profile;
    const page = context.pages()[0] || await context.newPage();
    page.on('console', (message) => report.console.push({type: message.type(), text: message.text()}));
    page.on('pageerror', (error) => report.pageErrors.push(String(error)));
    page.on('requestfailed', (request) => report.failedRequests.push({
      url: request.url(), error: request.failure()?.errorText,
    }));
    page.on('response', (response) => {
      if (response.status() >= 400)
        report.failedRequests.push({url: response.url(), status: response.status()});
    });
    if (values.url) {
      await page.goto(values.url, {timeout: 15000});
      await page.waitForFunction(() => window.probeResult !== null, undefined, {timeout: 30000});
      report.probe = await page.evaluate(() => window.probeResult);
      if (report.probe.status !== 'passed' || !report.probe.isolated ||
          !report.probe.worker || !report.probe.sharedMemory)
        throw new Error(`Wasm probe failed: ${JSON.stringify(report.probe)}`);
      if (report.pageErrors.length || report.failedRequests.length)
        throw new Error('Browser page or asset errors occurred.');
    } else {
      await page.goto('about:blank');
      report.userAgent = await page.evaluate(() => navigator.userAgent);
    }
    report.status = 'passed';
  } catch (error) {
    report.error = String(error.stack || error);
    process.exitCode = 1;
  } finally {
    if (context) await context.close();
    writeFileSync(values.output, JSON.stringify(report, null, 2) + '\n');
  }
}
