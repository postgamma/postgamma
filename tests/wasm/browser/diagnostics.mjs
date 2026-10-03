/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
export async function bounded(work, milliseconds = 2000) {
  let timer;
  try {
    return await Promise.race([work(), new Promise((_, reject) => {
      timer = setTimeout(() => reject(new Error('Diagnostic target did not respond')), milliseconds);
    })]);
  } catch (error) {return {error: String(error)};}
  finally {clearTimeout(timer);}
}

export async function controlSnapshots(page) {
  return Promise.all(page.workers().filter(worker => /\/src\/worker\/sql\.js(?:[?#]|$)/.test(worker.url()))
    .map(async worker => ({url: worker.url(), snapshot: await bounded(() =>
      worker.evaluate(async () => {
        if (typeof globalThis.postgammaDiagnostic !== 'function') throw new Error('Control diagnostics unavailable');
        return globalThis.postgammaDiagnostic();
      }))})));
}

export async function failureSnapshot(page) {
  const [view, controls] = await Promise.all([
    bounded(() => page.evaluate(async () => ({probe: window.probeResult,
      events: window.probeEvents || [], pglite: await window.pgliteSnapshot?.()}))),
    controlSnapshots(page),
  ]);
  return {page: view, controls};
}

export async function diagnosticGate(page, timeout = 180000) {
  await page.waitForFunction(() => (window.probeEvents || []).some(e => e.kind === 'diagnostic-hold'),
    undefined, {timeout});
  let before;
  const deadline = performance.now() + 10000;
  do {
    before = await controlSnapshots(page);
    if (before.some(row => row.snapshot.driver?.testHold === 1)) break;
    await new Promise(resolve => setTimeout(resolve, 10));
  } while (performance.now() < deadline);
  const held = before.find(row => row.snapshot.driver?.testHold === 1);
  if (!held || held.snapshot.currentCommand?.op !== 65530 || held.snapshot.driver?.state !== 1 ||
      !held.snapshot.threads?.some(thread => thread.id === held.snapshot.driver.thread &&
        thread.waitPoint === 'database-notification') || !held.snapshot.storage?.store)
    throw new Error('Missing held command, native waiter or OPFS owner diagnostic');
  const after = await controlSnapshots(page);
  if (!after.some(row => row.snapshot.driver?.testHold === 1 && row.snapshot.driver.sequence === held.snapshot.driver.sequence))
    throw new Error('Inspecting the driver changed its held command');
  const worker = page.workers().find(worker => worker.url() === held.url);
  await worker.evaluate(() => globalThis.postgammaResumeDiagnostic());
  return {before, after};
}
