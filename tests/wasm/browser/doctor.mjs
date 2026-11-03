/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
import {createRequire} from 'node:module';
import {writeFileSync} from 'node:fs';
import {resolve} from 'node:path';
import {parseArgs} from 'node:util';

const {values} = parseArgs({options: {
  root: {type: 'string'}, output: {type: 'string'},
  executable: {type: 'string'}, 'webkit-executable': {type: 'string'},
}});
const require = createRequire(resolve(values.root, 'wasm/package.json'));
const playwright = require('playwright');
const report = {status: 'failed', kernelValidated: false, engines: []};
const save = () => writeFileSync(values.output, JSON.stringify(report, null, 2) + '\n');

for (const name of ['chromium', 'firefox', 'webkit']) {
  const row = {name, status: 'failed'};
  report.engines.push(row);
  let browser;
  try {
    const override = name === 'chromium' ? values.executable :
      name === 'webkit' ? values['webkit-executable'] : undefined;
    browser = await playwright[name].launch({headless: true, timeout: 15000,
      ...(override ? {executablePath: override} : name === 'chromium' ? {channel: 'chromium'} : {})});
    const page = await browser.newPage();
    page.setDefaultTimeout(5000);
    await page.setContent('<title>PostGamma browser doctor</title>');
    if (await page.title() !== 'PostGamma browser doctor') throw new Error('Browser page did not respond');
    row.version = browser.version();
    row.status = 'passed';
  } catch (error) {
    row.error = String(error.stack || error);
  } finally {
    if (browser) await browser.close().catch((error) => {
      row.status = 'failed'; row.error = String(error.stack || error);
    });
    save();
  }
}
if (report.engines.every((row) => row.status === 'passed')) report.status = 'passed';
else process.exitCode = 1;
save();
