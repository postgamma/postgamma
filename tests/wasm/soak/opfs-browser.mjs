/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
import {createRequire} from 'node:module';
import {writeFileSync} from 'node:fs';
import {resolve} from 'node:path';
import assert from 'node:assert/strict';
import {MemoryDiagnostics} from './memory.mjs';
import {processes} from './processes.mjs';
const [root, url, profile, output, workload, iterationsText] = process.argv.slice(2);
const require = createRequire(resolve(root, 'wasm/package.json'));
const context = await require('playwright').chromium.launchPersistentContext(profile, {headless: true, channel: 'chromium'});
const page = context.pages()[0], iterations = Number(iterationsText), name = 'memory-' + crypto.randomUUID();
const report = {kind: 'postgamma.wasm-memory-diagnostic', status: 'in-progress', workload,
  iterations, browserVersion: context.browser().version(), samples: []};
const memory = await MemoryDiagnostics.open(context, page, output), started = performance.now();
try {
  await page.exposeFunction('memorySample', async event => {
    const {iteration, snapshot} = event;
    if (workload === 'opfs-churn') {
      for (const key of ['handles', 'virtualOpens', 'files', 'sizes', 'synced', 'removed'])
        assert.equal(snapshot[key], 0, 'Removed file ' + key);
      assert.equal(snapshot.namespaceNodes, 1);
    } else {
      assert.equal(snapshot.namespaceNodes, 2);
      assert.equal(snapshot.virtualOpens, 1);
      assert(snapshot.handles <= 1 && snapshot.files === 1);
    }
    const row = {...event, seconds: (performance.now() - started) / 1000, ...processes()};
    if (iteration % 1000 === 0 || iteration === iterations) row.memoryDiagnostic = await memory.sample(iteration);
    report.samples.push(row); writeFileSync(output, JSON.stringify(report, null, 2));
    if (iteration % 1000 === 0) console.log(JSON.stringify(row));
  });
  await page.goto(url);
  report.result = await page.evaluate(parameters => window.runMemoryWorkload(parameters), {name, workload, iterations});
  assert.equal(report.result.handles, 0); assert.equal(report.result.virtualOpens, 0);
  const deadline = performance.now() + 10000;
  while (page.workers().length && performance.now() < deadline)
    await new Promise(resolve => setTimeout(resolve, 10));
  assert.equal(page.workers().length, 0, 'Storage Worker physically retires within ten seconds');
  report.afterClose = processes();
  await new Promise(resolve => setTimeout(resolve, 15000));
  report.afterIdle = processes();
  await memory.finish();
  await page.evaluate(async name => {
    const origin = await navigator.storage.getDirectory(), databases = await origin.getDirectoryHandle('postgamma');
    await databases.removeEntry(name, {recursive: true});
  }, name);
  report.status = 'diagnostic-completed';
} catch (error) {report.status = 'failed'; report.error = String(error.stack || error); process.exitCode = 1;}
finally {
  report.elapsedSeconds = (performance.now() - started) / 1000;
  writeFileSync(output, JSON.stringify(report, null, 2)); await context.close();
}
