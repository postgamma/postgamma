/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
import {createRequire} from 'node:module';
import {writeFileSync} from 'node:fs';
import {resolve} from 'node:path';
import {parseArgs} from 'node:util';
import assert from 'node:assert/strict';
const {values} = parseArgs({options: {root: {type:'string'},url: {type:'string'},output: {type:'string'},
  engine: {type:'string', default:'chromium'}, executable: {type:'string'}, 'no-faults': {type:'boolean'}}});
const require = createRequire(resolve(values.root, 'wasm/package.json'));
const playwright = require('playwright');
const report = {status:'failed',engine:values.engine,playwright:require('playwright/package.json').version,cases:[]};
let browser;
try {
  browser = await playwright[values.engine].launch({headless:true,timeout:15000,
    ...(values.executable ? {executablePath:values.executable} : values.engine==='chromium' ? {channel:'chromium'} : {})});
  report.browserVersion = browser.version();
  const cases = [{name:'consumer'}, ...(values['no-faults'] ? [] : [
    {name:'identity',json:{buildId:'wrong-build'},expected:/build identity mismatch/},
    {name:'protocol',json:{protocolVersion:0},expected:/bridge protocol mismatch/},
    {name:'wasm',asset:'postgamma.wasm',expected:/asset integrity mismatch/},
    {name:'resources',asset:'postgamma.data',expected:/asset integrity mismatch/},
    {name:'worker',asset:'src/worker/sql.js',missing:true,expected:/Asset download failed/},
    {name:'isolation',expected:/cross-origin isolation/},
  ])];
  for (const scenario of cases) {
    const context = await browser.newContext();
    const row = {name:scenario.name,status:'failed',pageErrors:[],failedRequests:[],console:[]}; report.cases.push(row);
    try {
      const page = await context.newPage();
      page.on('pageerror', error => row.pageErrors.push(String(error)));
      page.on('console', message => row.console.push({type:message.type(),text:message.text()}));
      page.on('requestfailed', request => row.failedRequests.push(request.url()));
      if (scenario.json) await context.route('**/postgamma.assets.json', async route => {
        const response = await route.fetch(); const data = await response.json();
        await route.fulfill({response,json:{...data,...scenario.json}});
      });
      if (scenario.asset) await context.route('**/' + scenario.asset, route => route.fulfill({
        status:scenario.missing ? 404 : 200,body:'injected mismatched asset'}));
      if (scenario.name==='isolation') await page.route('**/consumer.html?*', async route => {
        const response = await route.fetch(); const headers = response.headers();
        delete headers['cross-origin-opener-policy']; delete headers['cross-origin-embedder-policy'];
        await route.fulfill({response,headers});
      });
      await page.goto(values.url + (scenario.expected ? '?fault=' + scenario.name : ''), {timeout:15000});
      try {await page.waitForFunction(() => window.probeResult != null, undefined, {timeout:120000});}
      catch (error) {row.progress = await page.evaluate(() => window.probeProgress); throw error;}
      row.probe = await page.evaluate(() => window.probeResult);
      if (scenario.expected) {
        assert.equal(row.probe.status,'rejected'); assert.match(row.probe.error,scenario.expected);
      } else {
        assert.equal(row.probe.status,'passed',row.probe.error); assert(row.probe.ticks >= 2);
        assert.deepEqual(row.failedRequests,[]);
      }
      assert.deepEqual(row.pageErrors,[]); row.status='passed';
    } catch (error) {row.error=String(error.stack||error); throw error;}
    finally {await context.close(); writeFileSync(values.output,JSON.stringify(report,null,2)+'\n');}
  }
  report.status='passed';
} catch (error) {report.error=String(error.stack||error); process.exitCode=1;}
finally {if(browser) await browser.close(); writeFileSync(values.output,JSON.stringify(report,null,2)+'\n');}
