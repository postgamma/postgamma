/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
import assert from 'node:assert/strict';
import {createRequire} from 'node:module';
import {writeFileSync} from 'node:fs';
import {resolve} from 'node:path';
import {parseArgs} from 'node:util';
const {values} = parseArgs({options: Object.fromEntries(['root', 'url', 'output', 'profile', 'engine', 'executable', 'case']
  .map(key => [key, {type: 'string'}]))});
const require = createRequire(resolve(values.root, 'wasm/package.json'));
const engine = values.engine ?? 'chromium';
assert(['chromium', 'firefox', 'webkit'].includes(engine));
const report = {kind: 'postgamma.wasm-multitab', status: 'failed', engine, cases: [],
  playwright: require('playwright/package.json').version, pageErrors: [],
  scope: 'Real same-origin tabs; two concurrent ephemeral instances are a bounded observation, not general capacity acceptance.'};
let context, anchor;
const save = () => writeFileSync(values.output, JSON.stringify(report, null, 2) + '\n');
const invoke = async (page, method, ...args) => {
  let timer;
  report.operation = method; save();
  try {
    return await Promise.race([page.evaluate(({method, args}) => window.multitab[method](...args), {method, args}),
      new Promise((_, reject) => {timer = setTimeout(() => reject(new Error('Cross-tab operation timed out: ' + method)), 90000);})]);
  } finally {clearTimeout(timer);}
};
async function page() {
  const p = await context.newPage();
  p.on('pageerror', error => report.pageErrors.push(String(error)));
  await p.goto(values.url, {timeout: 15000});
  await p.waitForFunction(() => window.multitab, undefined, {timeout: 15000});
  return p;
}
async function finish(p) {
  await p.waitForFunction(() => window.multitab.state().result?.status, undefined, {timeout: 180000});
  return (await invoke(p, 'state')).result;
}
async function open(p, options) {
  await invoke(p, 'begin', options);
  const result = await finish(p); assert.equal(result.status, 'opened', JSON.stringify(result)); return result;
}
async function waiting(observer, name, count = 1) {
  await observer.waitForFunction(async ({name, count}) =>
    (await navigator.locks.query()).pending.filter(lock => lock.name === 'postgamma:database:' + name).length === count,
    {name, count}, {timeout: 10000});
}
function locked(result, name, reason) {
  assert.equal(result.status, 'rejected', JSON.stringify(result));
  assert.equal(result.error.locked, true, 'Public DatabaseLockedError survives the Worker boundary');
  assert.equal(result.error.code, 'DATABASE_LOCKED'); assert.equal(result.error.databaseName, name);
  assert.equal(result.error.reason, reason);
  assert.equal(result.counts.memories, 0, 'A rejected owner never allocates shared linear memory');
  assert.equal(result.counts.pthreads, 0, 'A rejected owner never starts pthreads');
}
async function test(name, work) {
  if (values.case && values.case !== name) return;
  const row = {name, status: 'failed'}, started = performance.now(); report.cases.push(row);
  console.log('multitab:', engine, name); save();
  try {Object.assign(row, await work()); row.status = 'passed';}
  catch (error) {
    row.error = String(error.stack ?? error); console.log(row.error);
    row.pages = await Promise.all(context.pages().map(async p => {
      let timer;
      try {return await Promise.race([p.evaluate(() => ({url: location.href, readyState: document.readyState,
        state: window.multitab?.state(), lifecycle: window.multitab?.lifecycle()})),
        new Promise(resolve => {timer = setTimeout(() => resolve({error: 'Page inspection timed out'}), 3000);})]);}
      catch (cause) {return {error: String(cause)};}
      finally {clearTimeout(timer);}
    }));
  }
  finally {
    row.elapsedMs = performance.now() - started;
    console.log('multitab:', engine, name, row.status, Math.round(row.elapsedMs) + 'ms');
    // Firefox's persistent context needs its browser window to remain alive.
    // Keep one blank page; every page owning a database is still closed here.
    for (const p of context.pages()) if (p !== anchor) await p.close().catch(() => {});
    save();
  }
}
try {
  context = await require('playwright')[engine].launchPersistentContext(values.profile, {
    headless: true, timeout: 15000, ignoreDefaultArgs: ['--disable-back-forward-cache'],
    // The pinned Firefox driver cannot grant persistent-storage through
    // grantPermissions. Its real permission UI honors this profile default.
    ...(engine === 'firefox' && (!values.case || values.case === 'storage-persistence') ?
      {firefoxUserPrefs: {'permissions.default.persistent-storage': 1}} : {}),
    ...(values.executable ? {executablePath: values.executable} : engine === 'chromium' ? {channel: 'chromium'} : {}),
  });
  report.browserVersion = context.browser().version();
  anchor = context.pages()[0] ?? await context.newPage();
  context.setDefaultTimeout(15000); context.setDefaultNavigationTimeout(15000);
  await test('exclusive-lock', async () => {
    const a = await page(), b = await page(), name = 'tabs-' + crypto.randomUUID();
    await open(a, {name}); await invoke(a, 'init');
    await invoke(b, 'begin', {name, lock: 'fail'});
    const busy = await finish(b); locked(busy, name, 'busy');
    await invoke(b, 'begin', {name, lockTimeoutMs: 200});
    const timeout = await finish(b); locked(timeout, name, 'timeout');
    await invoke(b, 'begin', {name}); await waiting(a, name);
    const pending = await invoke(b, 'state');
    assert.equal(pending.counts.memories, 0); assert.equal(pending.counts.pthreads, 0);
    await invoke(a, 'close');
    assert.equal((await finish(b)).status, 'opened');
    assert.deepEqual(await invoke(b, 'values'), {ack: '2', pending: '0'});
    await invoke(b, 'close');
    return {busy, timeout, pending};
  });
  await test('cancelled-waiter', async () => {
    const a = await page(), b = await page(), c = await page(), name = 'cancel-' + crypto.randomUUID();
    await invoke(b, 'begin', {name, preabort: true});
    const preabort = await finish(b);
    assert.equal(preabort.error.name, 'AbortError'); assert.deepEqual(preabort.counts, {memories: 0, workers: 0, pthreads: 0});
    await open(a, {name});
    await invoke(b, 'begin', {name, lockTimeoutMs: 20000}); await waiting(a, name);
    await invoke(c, 'begin', {name, lockTimeoutMs: 20000}); await waiting(a, name, 2);
    await invoke(b, 'abort');
    const aborted = await finish(b); assert.equal(aborted.error.name, 'AbortError');
    assert.equal(aborted.counts.memories, 0); assert.equal(aborted.counts.pthreads, 0);
    await waiting(a, name); await invoke(a, 'close');
    assert.equal((await finish(c)).status, 'opened', 'Next live waiter acquires after cancellation');
    await invoke(c, 'close'); await open(a, {name, lock: 'fail'}); await invoke(a, 'close');
    return {preabort, aborted};
  });
  await test('cancelled-startup', async () => {
    const a = await page(), b = await page(), name = 'startup-' + crypto.randomUUID();
    let reached, release;
    const requested = new Promise(resolve => {reached = resolve;});
    const released = new Promise(resolve => {release = resolve;});
    await a.route('**/*.wasm', async route => {reached(); await released; await route.abort();});
    let timer;
    try {
      await invoke(a, 'begin', {name});
      await Promise.race([requested, new Promise((_, reject) => {
        timer = setTimeout(() => reject(new Error('Kernel download did not reach startup gate')), 15000);
      })]);
      const held = await b.evaluate(async name => (await navigator.locks.query()).held.some(lock =>
        lock.name === 'postgamma:database:' + name), name);
      assert(held, 'Ownership precedes kernel download and allocation');
      await invoke(a, 'abort');
      const aborted = await finish(a); assert.equal(aborted.error.name, 'AbortError');
      assert.equal(aborted.counts.memories, 0); assert.equal(aborted.counts.pthreads, 0);
      release();
      await open(b, {name}); await invoke(b, 'close');
      return {aborted, heldBeforeDownload: held};
    } finally {clearTimeout(timer); release();}
  });
  for (const boundary of ['flush-before', 'flush-after']) await test('tab-close-' + boundary, async () => {
    const a = await page(), b = await page(), name = 'crash-' + crypto.randomUUID();
    await open(a, {name, fault: true}); await invoke(a, 'init');
    const gate = await invoke(a, 'armCommit', boundary);
    await invoke(b, 'begin', {name, lockTimeoutMs: 20000}); await waiting(a, name);
    await a.close();
    const reopened = await finish(b); assert.equal(reopened.status, 'opened', JSON.stringify(reopened));
    const rows = await invoke(b, 'values'); assert.equal(rows.ack, '2'); assert(['0', '2'].includes(rows.pending));
    await invoke(b, 'close'); return {gate, rows, interruption: 'page.close without Database.close'};
  });
  await test('reload', async () => {
    const a = await page(), name = 'reload-' + crypto.randomUUID();
    await open(a, {name}); await invoke(a, 'init');
    for (let round = 0; round < 3; round++) {
      await invoke(a, 'sql', "BEGIN; INSERT INTO ledger VALUES(3,'uncommitted')");
      await a.reload(); await a.waitForFunction(() => window.multitab);
      await open(a, {name}); assert.deepEqual(await invoke(a, 'values'), {ack: '2', pending: '0'});
      assert.deepEqual(await invoke(a, 'sql', 'SELECT count(*) FROM ledger'), [['2']]);
    }
    await invoke(a, 'close'); return {rounds: 3};
  });
  await test('navigation-and-back', async () => {
    const a = await page(), b = await page(), name = 'navigation-' + crypto.randomUUID();
    await open(a, {name}); await invoke(a, 'init');
    const destination = new URL('multitab-empty.html', values.url).href;
    // First return without a competing lock request: contention itself can
    // evict the cached owner and conceal whether restoration ever worked.
    await a.goto(destination);
    // A bfcache restoration commits a navigation without a new load event.
    await a.goBack({waitUntil: 'commit'}); await a.waitForFunction(() => window.multitab);
    const uncontendedLifecycle = await invoke(a, 'lifecycle');
    const uncontendedRestore = uncontendedLifecycle.filter(event => event.type === 'pageshow').at(-1)?.persisted === true;
    if (uncontendedRestore) {
      await assert.rejects(invoke(a, 'sql', 'SELECT 1'), /discarded on entering bfcache/);
      await invoke(a, 'close').catch(() => {});
    }
    await open(a, {name});
    assert.deepEqual(await invoke(a, 'values'), {ack: '2', pending: '0'});
    await a.goto(destination);
    await open(b, {name}); assert.deepEqual(await invoke(b, 'values'), {ack: '2', pending: '0'});
    await invoke(b, 'close');
    await a.goBack({waitUntil: 'commit'}); await a.waitForFunction(() => window.multitab);
    const lifecycle = await invoke(a, 'lifecycle');
    const restored = lifecycle.filter(event => event.type === 'pageshow').at(-1)?.persisted === true;
    // A restored page keeps JS objects, but its previous DB must be unusable.
    if (restored) {
      let stale = false;
      try {await invoke(a, 'sql', 'SELECT 1');} catch (error) {
        // A harness deadline is a hang, not an SDK rejection of a stale handle.
        if (String(error).includes('Cross-tab operation timed out')) throw error;
        stale = true;
      }
      assert(stale, 'Restored page must not reuse a database whose ownership was released');
      await invoke(a, 'close').catch(() => {});
    }
    await open(a, {name}); assert.deepEqual(await invoke(a, 'values'), {ack: '2', pending: '0'});
    await invoke(a, 'close'); return {lifecycle, uncontendedLifecycle,
      bfcacheRestoreExercised: uncontendedRestore || restored, restoredAfterOwnershipTransfer: restored};
  });
  await test('two-independent-instances', async () => {
    const a = await page(), b = await page();
    const instances = await Promise.all([open(a, {}), open(b, {})]);
    for (const instance of instances) {
      assert.equal(instance.counts.memories, 1);
      assert.equal(instance.counts.workers, 16); assert.equal(instance.counts.pthreads, 15);
      assert(instance.linearMemory >= 128 * 1024 * 1024 && instance.linearMemory <= 512 * 1024 * 1024);
    }
    await Promise.all([invoke(a, 'sql', "CREATE TABLE private_data(value text); INSERT INTO private_data VALUES('a')"),
      invoke(b, 'sql', "CREATE TABLE private_data(value text); INSERT INTO private_data VALUES('b')")]);
    assert.deepEqual(await invoke(a, 'sql', 'SELECT * FROM private_data'), [['a']]);
    assert.deepEqual(await invoke(b, 'sql', 'SELECT * FROM private_data'), [['b']]);
    await invoke(a, 'close'); assert.deepEqual(await invoke(b, 'sql', 'SELECT * FROM private_data'), [['b']]);
    await invoke(b, 'close'); return {instances, instanceCount: 2};
  });
  await test('database-management', async () => {
    const a = await page(), b = await page(), name = 'delete-' + crypto.randomUUID();
    assert.equal(await invoke(b, 'delete', name), false);
    await open(a, {name}); await invoke(a, 'init');
    assert((await invoke(b, 'list')).includes(name));
    await assert.rejects(invoke(b, 'delete', name, {lock: 'fail'}), /already has a live owner/);
    await assert.rejects(invoke(b, 'delete', name, {lockTimeoutMs: 100}), /Timed out waiting/);
    assert.deepEqual(await invoke(a, 'values'), {ack: '2', pending: '0'});
    const deleting = invoke(b, 'delete', name);
    await waiting(a, name); await invoke(a, 'close');
    assert.equal(await deleting, true);
    assert(!(await invoke(b, 'list')).includes(name));
    assert.equal(await invoke(b, 'delete', name), false);
    await open(a, {name}); await invoke(a, 'init'); await invoke(a, 'close');
    assert.equal(await invoke(b, 'delete', name), true);
    return {deletedAfterOwnerClosed: true, nameReusable: true};
  });
  await test('pagehide-discard', async () => {
    const a = await page(), b = await page(), name = 'pagehide-' + crypto.randomUUID();
    await open(a, {name}); await invoke(a, 'init');
    await invoke(a, 'pagehide', false);
    assert.deepEqual(await invoke(a, 'values'), {ack: '2', pending: '0'});
    await invoke(a, 'sql', "BEGIN; INSERT INTO ledger VALUES(3,'pending')");
    await invoke(a, 'pagehide', true);
    await assert.rejects(invoke(a, 'sql', 'SELECT 1'), /discarded on entering bfcache/);
    await open(b, {name}); assert.deepEqual(await invoke(b, 'values'), {ack: '2', pending: '0'});
    await invoke(b, 'close'); await invoke(a, 'close').catch(() => {});
    await open(a, {name, bfcache: 'preserve'});
    await invoke(a, 'pagehide', true);
    assert.deepEqual(await invoke(a, 'values'), {ack: '2', pending: '0'});
    await assert.rejects(invoke(b, 'delete', name, {lock: 'fail'}), /already has a live owner/);
    await invoke(a, 'close'); await invoke(b, 'delete', name);
    return {syntheticPagehide: true, defaultDiscard: true, explicitPreserve: true};
  });
  await test('storage-persistence', async () => {
    const a = await page();
    const before = await invoke(a, 'persistence');
    assert(['unsupported', 'best-effort', 'persistent'].includes(before));
    const after = await invoke(a, 'persistence', true);
    assert(['unsupported', 'best-effort', 'persistent'].includes(after));
    if (engine === 'firefox') assert.equal(after, 'persistent', 'Firefox profile allows the real persistence request');
    assert.equal(await invoke(a, 'persistence'), after === 'unsupported' ? before : after);
    return {before, after, permissionPreset: engine === 'firefox' ? 'firefox-default-allow' : 'browser-default'};
  });
  assert(report.cases.length > 0);
  assert(report.cases.every(row => row.status === 'passed'), 'Cross-tab cases failed');
  assert.equal(report.pageErrors.length, 0, JSON.stringify(report.pageErrors));
  report.status = 'passed';
} catch (error) {report.error = String(error.stack ?? error); process.exitCode = 1;}
finally {save(); await context?.close();}
