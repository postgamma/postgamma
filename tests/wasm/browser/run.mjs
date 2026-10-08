/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
import {createRequire} from 'node:module';
import {readFileSync, writeFileSync} from 'node:fs';
import {dirname, resolve} from 'node:path';
import {parseArgs} from 'node:util';
import {classifyPageErrors} from './evidence.mjs';
import {bounded, diagnosticGate, failureSnapshot} from './diagnostics.mjs';

const {values} = parseArgs({options: {
  root: {type: 'string'},
  url: {type: 'string'},
  output: {type: 'string'},
  describe: {type: 'boolean'},
  executable: {type: 'string'},
  engine: {type: 'string', default: 'chromium'},
  profile: {type: 'string'},
  'timeout-ms': {type: 'string'},
  'initial-timeout-ms': {type: 'string'},
  repetitions: {type: 'string'},
}});
const require = createRequire(resolve(values.root, 'wasm/package.json'));
if (!['chromium', 'firefox', 'webkit'].includes(values.engine)) throw new Error('Unsupported browser engine');
const browserType = require('playwright')[values.engine];
const report = {
  status: values.describe ? 'not-run' : 'failed',
  engine: values.engine,
  playwright: require('playwright/package.json').version,
  executable: values.executable || browserType.executablePath(),
  browserRevisions: JSON.parse(readFileSync(resolve(dirname(require.resolve('playwright-core/package.json')), 'browsers.json'))).browsers,
  console: [],
  pageErrors: [],
  pageErrorDetails: [],
  failedRequests: [],
  timeoutMs: Number(values['timeout-ms'] || 30000),
  initialTimeoutMs: Number(values['initial-timeout-ms'] || values['timeout-ms'] || 30000),
  repetitions: Number(values.repetitions || 0),
  completedIterations: 0,
};
if (!Number.isSafeInteger(report.timeoutMs) || report.timeoutMs < 1 || report.timeoutMs > 600000)
  throw new Error('Invalid browser case timeout');
if (!Number.isSafeInteger(report.initialTimeoutMs) || report.initialTimeoutMs < report.timeoutMs || report.initialTimeoutMs > 600000)
  throw new Error('Invalid browser initial fixture timeout');
if (!Number.isSafeInteger(report.repetitions) || report.repetitions < 0 || report.repetitions > 100)
  throw new Error('Invalid browser repetition count');
if (values.describe) {
  console.log(JSON.stringify(report));
} else {
  let context, page;
  try {
    context = await browserType.launchPersistentContext(values.profile, {
      headless: true,
      ...(values.executable ? {executablePath: values.executable} : values.engine === 'chromium' ? {channel: 'chromium'} : {}),
      timeout: 15000,
    });
    report.browserVersion = context.browser().version();
    report.profile = values.profile;
    page = context.pages()[0] || await context.newPage();
    page.on('console', (message) => report.console.push({type: message.type(), text: message.text()}));
    page.on('pageerror', (error) => {
      report.pageErrors.push(String(error));
      report.pageErrorDetails.push({name: error.name, message: error.message, stack: error.stack || ''});
    });
    page.on('requestfailed', (request) => report.failedRequests.push({
      url: request.url(), error: request.failure()?.errorText,
    }));
    page.on('response', (response) => {
      if (response.status() >= 400)
        report.failedRequests.push({url: response.url(), status: response.status()});
    });
    if (values.url) {
      await page.goto(values.url, {timeout: 15000});
      if (new URL(values.url).searchParams.get('scenario') === 'diagnostics')
        report.diagnosticGate = await diagnosticGate(page, report.initialTimeoutMs);
      const started = performance.now();
      // OPFS creation and the deliberate 90s startup timeout are separate
      // bounded phases. A single 180s wall clock can expire during a valid
      // immediate reopen on a slow filesystem.
      if (new URL(values.url).searchParams.get('scenario') === 'connect-progress') {
        report.connectionPhaseBudgets = {initialMs: report.initialTimeoutMs, discardMs: 105000, recoveryMs: 30000};
        for (const [phase, timeout] of [['ready', report.initialTimeoutMs], ['discarded', 105000], [null, 30000]]) {
          await page.waitForFunction(name => window.probeResult !== null || name !== null &&
            (window.probeEvents || []).some(e => e.kind === 'connect-watchdog-phase' && e.phase === name),
            phase, {timeout});
          const progress = await page.evaluate(() => ({done: window.probeResult !== null, events: window.probeEvents || []}));
          report.events = progress.events;
          writeFileSync(values.output, JSON.stringify(report, null, 2) + '\n');
          if (progress.done) break;
        }
      }
      // Bound every round separately. Keep partial progress on disk even if
      // a later renderer crash prevents reading the page's final evidence.
      for (let iteration = 1; iteration <= report.repetitions; iteration++) {
        await page.waitForFunction(n => window.probeResult !== null ||
          (window.probeEvents || []).filter(e => e.kind === 'fatal-iteration').length >= n,
          iteration, {timeout: iteration === 1 ? report.initialTimeoutMs : report.timeoutMs});
        const progress = await page.evaluate(() => ({done: window.probeResult !== null,
          count: (window.probeEvents || []).filter(e => e.kind === 'fatal-iteration').length}));
        report.completedIterations = progress.count;
        writeFileSync(values.output, JSON.stringify(report, null, 2) + '\n');
        if (progress.done) break;
      }
      await page.waitForFunction(() => window.probeResult !== null, undefined,
        {timeout: report.repetitions ? report.timeoutMs : report.initialTimeoutMs});
      report.elapsedMs = performance.now() - started;
      report.probe = await page.evaluate(() => window.probeResult);
      if (report.probe.status !== 'passed' || !report.probe.isolated ||
          !report.probe.worker || !report.probe.sharedMemory)
        throw new Error(`Wasm probe failed: ${report.probe.error || JSON.stringify({
          status: report.probe.status, isolated: report.probe.isolated,
          worker: report.probe.worker, sharedMemory: report.probe.sharedMemory,
        })}`);
      const classified = classifyPageErrors(report.pageErrorDetails, report.probe, values.url);
      report.expectedPageErrors = classified.expected;
      report.unexpectedPageErrors = classified.unexpected;
      if (report.unexpectedPageErrors.length || report.failedRequests.length)
        throw new Error('Browser page or asset errors occurred.');
    } else {
      await page.goto('about:blank');
      report.userAgent = await page.evaluate(() => navigator.userAgent);
    }
    report.status = 'passed';
  } catch (error) {
    report.error = String(error.stack || error);
    if (page && !page.isClosed()) report.failureSnapshot = await failureSnapshot(page);
    process.exitCode = 1;
  } finally {
    if (values.url && page && !page.isClosed()) {
      try {
        const partial = await bounded(() => page.evaluate(() => ({
          probe: window.probeResult, events: window.probeEvents || [],
        })));
        report.events = partial.events || [];
        if (partial.error) report.evidenceError = partial.error;
        if (partial.probe) report.probe = partial.probe;
      } catch (error) {
        report.evidenceError = String(error);
      }
    }
    writeFileSync(values.output, JSON.stringify(report, null, 2) + '\n');
    if (context) await context.close();
  }
}
