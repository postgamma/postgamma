/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
import {createRequire} from 'node:module';
import {readFileSync, writeFileSync, existsSync} from 'node:fs';
import {dirname, resolve} from 'node:path';
import {parseArgs} from 'node:util';
import {scenarios, assess} from './deployment-evidence.mjs';

const {values} = parseArgs({options: {
  root: {type: 'string'}, url: {type: 'string'}, output: {type: 'string'},
  describe: {type: 'boolean'}, executable: {type: 'string'}, profile: {type: 'string'},
  'webkit-executable': {type: 'string'},
}});
const require = createRequire(resolve(values.root, 'wasm/package.json'));
const playwright = require('playwright');
const metadata = {
  playwright: require('playwright/package.json').version,
  browserRevisions: JSON.parse(readFileSync(resolve(dirname(require.resolve('playwright-core/package.json')), 'browsers.json'))).browsers,
  engines: ['chromium', 'firefox', 'webkit'].map((name) => ({name, executable: playwright[name].executablePath()})),
};
const report = {status: 'failed', kind: 'postgamma.wasm-deployment-matrix', kernelValidated: false,
  browserMatrixComplete: false, tools: metadata, engines: []};
const save = () => writeFileSync(values.output, JSON.stringify(report, null, 2) + '\n');
if (values.describe) {
  console.log(JSON.stringify(metadata));
} else {
  try {
    for (const engine of metadata.engines) {
      const row = {name: engine.name, executable: engine.executable, status: 'not-run', scenarios: []};
      report.engines.push(row);
      let browser;
      try {
        const override = engine.name === 'chromium' ? values.executable :
                         engine.name === 'webkit' ? values['webkit-executable'] : null;
        row.launcher = override || engine.executable;
        if (!existsSync(row.launcher)) throw new Error('Browser is missing; run make wasm-browser-setup');
        browser = await playwright[engine.name].launch({headless: true, timeout: 15000,
          ...(override ? {executablePath: override} : engine.name === 'chromium' ? {channel: 'chromium'} : {})});
        row.version = browser.version();
        for (const scenario of scenarios) {
          const item = {name: scenario.name, expected: scenario.expected, status: 'not-run',
            console: [], requests: [], pageErrors: []};
          row.scenarios.push(item);
          if (row.status === 'unsupported') {
            item.reason = 'Required capabilities are absent in the baseline; dependent fault cases were not run.';
            continue;
          }
          const context = await browser.newContext();
          try {
            const page = await context.newPage();
            page.on('console', (message) => item.console.push({type: message.type(), text: message.text()}));
            page.on('pageerror', (error) => item.pageErrors.push(String(error)));
            page.on('requestfailed', (request) => item.requests.push({url: request.url(), error: request.failure()?.errorText}));
            page.on('response', (response) => {
              if (response.status() >= 400) item.requests.push({url: response.url(), status: response.status()});
            });
            if (scenario.capability) await page.addInitScript((name) => {
              Object.defineProperty(globalThis, name, {value: undefined, configurable: true});
            }, scenario.capability);
            if (scenario.remove) await page.route('**/deployment.html', async (route) => {
              const response = await route.fetch();
              const headers = response.headers();
              for (const key of scenario.remove) delete headers[key];
              await route.fulfill({response, headers});
            });
            if (scenario.asset) await page.route(`**/${scenario.asset}`, (route) => route.fulfill({
              status: scenario.corrupt ? 200 : 404,
              contentType: scenario.corrupt ? 'application/wasm' : 'text/plain',
              body: scenario.corrupt ? Buffer.from('invalid wasm module') : 'injected missing probe asset',
            }));
            const response = await page.goto(values.url, {timeout: 10000});
            item.headers = response.headers();
            await page.waitForFunction(() => window.probeResult !== null, undefined, {timeout: 10000});
            item.result = await page.evaluate(() => window.probeResult);
            item.uiResponsive = await page.evaluate(() => new Promise((resolve) => setTimeout(() => resolve(true), 20)));
            item.status = assess(scenario, item.result, item.requests, item.pageErrors);
            if (scenario.name === 'isolated') row.status = item.status;
          } catch (error) {
            item.status = 'failed';
            item.error = String(error.stack || error);
          } finally {
            await context.close();
            save();
          }
        }
        if (row.scenarios.some((s) => s.status === 'failed')) row.status = 'failed';
      } catch (error) {
        row.error = String(error.stack || error);
        if (browser) row.status = 'failed';
      } finally {
        if (browser) await browser.close();
        save();
      }
    }
    report.browserMatrixComplete = report.engines.length === 3 && report.engines.every((r) => ['passed', 'unsupported'].includes(r.status));
    if (!report.browserMatrixComplete || report.engines[0].status !== 'passed')
      throw new Error('Browser deployment matrix incomplete or failed; inspect per-engine evidence.');
    report.status = 'passed';
  } catch (error) {
    report.error = String(error.stack || error);
    process.exitCode = 1;
  } finally { save(); }
}
