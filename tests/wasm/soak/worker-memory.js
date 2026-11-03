/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
const parameters = new URL(location.href).searchParams;
const iterations = Number(parameters.get('iterations')), waves = Number(parameters.get('waves'));
const idleSeconds = Number(parameters.get('idle'));
const counts = {created: 0, terminated: 0, finalized: 0};
const finalized = new FinalizationRegistry(() => counts.finalized++);
const delay = ms => new Promise(resolve => setTimeout(resolve, ms));
const emit = async value => {
  const response = await fetch('/__event', {method: 'POST', body: JSON.stringify({...value, counts: {...counts}})});
  if (!response.ok) throw new Error('Diagnostic event rejected: ' + response.status);
};
async function cycle() {
  // Match the application's control plus pthread Worker count, without
  // loading its SDK, Wasm, PostgreSQL, OPFS or a MessageChannel broker.
  const workers = [];
  try {
    await Promise.all(Array.from({length: 14}, () => new Promise((resolve, reject) => {
      const worker = new Worker('/idle-worker.js', {type: 'module'});
      workers.push(worker); counts.created++; finalized.register(worker, undefined);
      worker.onmessage = () => resolve();
      worker.onerror = event => {event.preventDefault(); reject(new Error(event.message));};
    })));
  } finally {
    for (const worker of workers) {
      worker.onmessage = null; worker.onerror = null;
      worker.terminate(); counts.terminated++;
    }
  }
}
try {
  await emit({phase: 'ready', userAgent: navigator.userAgent});
  for (let wave = 1; wave <= waves; wave++) {
    for (let iteration = 1; iteration <= iterations; iteration++) {
      await cycle();
      if (iteration % 10 === 0 || iteration === iterations) await emit({phase: 'cycle', wave, iteration});
    }
    const idle = wave === waves ? Math.max(120, idleSeconds) : idleSeconds;
    await emit({phase: 'idle', wave, idleAt: 0});
    for (let idleAt = 0; idleAt < idle;) {
      const step = Math.min(10, idle - idleAt);
      await delay(step * 1000); idleAt += step;
      await emit({phase: 'idle', wave, idleAt});
    }
  }
  await emit({phase: 'done'});
} catch (error) {
  await emit({phase: 'error', error: String(error.stack || error)});
}
