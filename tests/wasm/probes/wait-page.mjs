/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
window.probeResult = null;
window.probeEvents = [];
const seed = Number(new URL(location.href).searchParams.get('seed') || '1');
const report = {status: 'failed', isolated: crossOriginIsolated, worker: false, sharedMemory: false,
  seed, cases: [], raceOutcomes: {notified: 0, cancelled: 0}};
const require = (condition, message) => { if (!condition) throw new Error(message); };
const delay = (ms) => new Promise((resolve) => setTimeout(resolve, ms));
const A = {notify: 1, cancel: 2, close: 3, arm: 4, disarm: 5, timer: 6, poke: 7};
const R = {notify: 1, timer: 2, cancel: 3, close: 4, own: 5};
const G = {start: 1, check: 2, timeout: 4};
report.legend = {
  actions: A, outcomes: {...R, pending: 0, error: 6},
  stages: {starting: 0, gateStart: 1, gateCheck: 2, waiting: 3, gateTimeout: 4, finished: 5, joined: 6},
  trace: {start: 0, gate: 1, wait: 2, return: 3, action: 4, stale: 5, finish: 6, join: 7},
};
const pending = new Map();
let sequence = 0, requestId = 0, worker, readyTimeout, fatal;
const record = (event) => window.probeEvents.push({timeMs: performance.now(), ...event});

async function rpc(command, args = {}) {
  if (fatal) throw fatal;
  const id = ++sequence;
  let timer;
  const message = {id, command};
  for (const key of ['slot', 'request', 'ownMs', 'timerMs', 'gate', 'action', 'timer', 'value', 'full', 'actions', 'gapMs'])
    if (args[key] !== undefined) message[key] = args[key];
  record({direction: 'send', ...message});
  try {
    return await new Promise((resolve, reject) => {
      pending.set(id, {resolve, reject});
      timer = setTimeout(() => reject(new Error(`${command}: control response timed out`)), 5000);
      worker.postMessage(message);
    });
  } finally {
    clearTimeout(timer);
    pending.delete(id);
  }
}

async function start(name, {slot = 0, ownMs = -1, timerMs = -1, gate = 0} = {}) {
  const c = {name, slot, request: ++requestId, status: 'running'};
  report.cases.push(c);
  const result = await rpc('start', {...c, ownMs, timerMs, gate});
  c.initial = result.snapshot;
  require(result.rc === 0, `${name}: pthread start returned ${result.rc}`);
  return c;
}

async function until(c, predicate) {
  const end = performance.now() + 5000;
  do {
    const state = await rpc('snapshot', c);
    require(state.error === 0, `${c.name}: C wait error ${state.error}`);
    if (predicate(state)) return state;
    await delay(5);
  } while (performance.now() < end);
  throw new Error(`${c.name}: state transition timed out`);
}

const atGate = (c, stage) => until(c, (s) => s.stage === stage);
const parked = (c) => until(c, (s) => s.stage === 3 && s.waits > 0);
async function action(c, action, value = 0, timer = 0, stale = false) {
  const result = await rpc('action', {...c, action, value, timer});
  require(stale ? result.rc === report.clocks.errors.stale : result.rc >= 0,
    `${c.name}: action ${action} returned ${result.rc}`);
  return result;
}
async function gate(c) {
  require((await rpc('gate', c)).rc >= 0, `${c.name}: gate release failed`);
}

async function hold(cases, durationMs) {
  const started = performance.now(), latencies = [];
  do {
    const sent = performance.now();
    for (const c of cases) {
      const state = await rpc('snapshot', c);
      require(state.active && !state.done && state.completions === 0 && state.error === 0,
        `${c.name}: completed before its explicit event`);
    }
    latencies.push(performance.now() - sent);
    await delay(25);
  } while (performance.now() - started < durationMs || latencies.length < 3);
  return {heldMs: performance.now() - started, responses: latencies.length * cases.length, latencyMs: latencies};
}

async function finish(c, expected) {
  await until(c, (s) => s.done);
  require((await rpc('join', c)).rc === 0, `${c.name}: join failed`);
  c.result = await rpc('snapshot', {...c, full: 1});
  const s = c.result;
  require((Array.isArray(expected) ? expected : [expected]).includes(s.outcome), `${c.name}: wrong outcome ${s.outcome}`);
  require(s.completions === 1 && s.error === 0 && s.traceDropped === 0, `${c.name}: incomplete or duplicated outcome`);
  require(BigInt(s.entryNs) >= BigInt(s.startNs) && BigInt(s.finishNs) >= BigInt(s.entryNs) &&
    BigInt(s.snapshotNs) >= BigInt(s.finishNs), `${c.name}: inconsistent clocks across Workers`);
  if (s.outcome === R.timer) require(BigInt(s.finishPgUs) >= BigInt(s.timerPgUs), `${c.name}: early virtual timeout`);
  if (s.outcome === R.own) require(BigInt(s.finishNs) >= BigInt(s.ownNs), `${c.name}: early monotonic timeout`);
  c.status = 'completed';
  // Allow the SDK to return the joined Worker to the precreated pool.
  await delay(5);
  return s;
}

try {
  require(crossOriginIsolated && typeof SharedArrayBuffer !== 'undefined', 'cross-origin isolation required');
  require(Number.isInteger(seed) && seed > 0 && seed <= 0xffffffff, 'seed must be a nonzero uint32');
  worker = new Worker('./wait-worker.mjs', {type: 'module'});
  const ready = await new Promise((resolve, reject) => {
    readyTimeout = setTimeout(() => reject(new Error('wait Worker startup timed out')), 15000);
    const fail = (message) => {
      fatal = new Error(message);
      reject(fatal);
      for (const request of pending.values()) request.reject(fatal);
    };
    worker.onerror = (event) => fail(event.message);
    worker.onmessage = ({data}) => {
      record({direction: 'receive', data});
      if (data.kind === 'ready') resolve(data);
      else if (data.kind === 'fatal') fail(data.error);
      else if (pending.has(data.id)) {
        const request = pending.get(data.id);
        data.error ? request.reject(new Error(data.error)) : request.resolve(data.result);
      }
    };
  });
  clearTimeout(readyTimeout);
  require(ready.isolated && ready.worker && ready.sharedMemory, 'isolated shared Worker required');
  report.worker = ready.worker;
  report.sharedMemory = ready.sharedMemory;

  const wallBefore = BigInt(Date.now()) * 1000n;
  report.clocks = await rpc('clocks');
  const wallAfter = BigInt(Date.now()) * 1000n;
  const clocks = report.clocks;
  require(clocks.failures === 0 && clocks.cases.length === 13, 'clock conversion cases failed');
  require(BigInt(clocks.unixUs) - BigInt(clocks.pgUs) === 946684800000000n, 'PostgreSQL epoch mismatch');
  require(BigInt(clocks.unixUs) >= wallBefore - 1n && BigInt(clocks.unixUs) <= wallAfter + 1n, 'realtime provider mismatch');
  require(BigInt(clocks.monotonicNs) >= BigInt(Math.floor(clocks.beforeMs * 1e6)) - 1000n &&
    BigInt(clocks.monotonicNs) <= BigInt(Math.ceil(clocks.afterMs * 1e6)) + 1000n,
    'monotonic provider does not match the SDK pthread clock origin');

  const long = await start('long-wait-and-spurious-wake', {ownMs: 30000});
  await parked(long);
  // A positive wake count confirms an actual futex waiter, not merely a JS Promise.
  let woke = 0;
  for (let attempt = 0; attempt < 20 && !woke; attempt++) {
    woke = (await action(long, A.poke)).rc;
    if (!woke) await delay(5);
  }
  require(woke === 1, 'could not observe an actual parked futex waiter');
  await until(long, (s) => s.waits >= 2 && s.stage === 3);
  long.control = await hold([long], 1000);
  await action(long, A.notify, 123);
  let result = await finish(long, R.notify);
  require(result.timedOut === 0 && result.lastBudgetMs > 28000 && result.pokeWoken >= 1,
    'long wait used short polling or treated a spurious wake as completion');

  const pre = await start('notify-before-wait-and-coalescing', {gate: G.start});
  await atGate(pre, G.start);
  await action(pre, A.notify, 10);
  await action(pre, A.notify, 11);
  await gate(pre);
  result = await finish(pre, R.notify);
  require(result.waits === 0 && result.payload === 11, 'preexisting pending state was lost');

  const gap = await start('notify-between-check-and-sleep', {gate: G.check});
  await atGate(gap, G.check);
  await action(gap, A.notify, 42);
  await gate(gap);
  result = await finish(gap, R.notify);
  require(result.mismatch >= 1 && result.payload === 42, 'expected-value wait did not close the notification gap');

  await finish(await start('own-deadline-earlier', {ownMs: 90, timerMs: 5000}), R.own);
  await finish(await start('virtual-deadline-earlier', {ownMs: 5000, timerMs: 90}), R.timer);
  await finish(await start('already-expired-deadline', {timerMs: 0}), R.timer);

  const armed = await start('arm-shortens-infinite-wait');
  await parked(armed);
  await action(armed, A.arm, 80);
  await finish(armed, R.timer);

  const blocked = await start('deadline-with-control-js-blocked');
  await parked(blocked);
  let uiTicks = 0;
  const uiTimer = setInterval(() => uiTicks++, 20);
  try { blocked.controlBlocked = await rpc('block-control', blocked); }
  finally { clearInterval(uiTimer); }
  result = await finish(blocked, R.timer);
  const diagnostic = blocked.controlBlocked;
  require(diagnostic.rc >= 0 && diagnostic.blockResult === 'timed-out' && uiTicks >= 3, 'control-block diagnostic failed');
  require(BigInt(result.finishNs) > BigInt(diagnostic.before.monotonicNs) &&
    BigInt(result.finishNs) < BigInt(diagnostic.after.monotonicNs), 'deadline depended on the blocked control JS event loop');
  blocked.uiTicks = uiTicks;

  const reset = await start('reset-to-later-deadline', {timerMs: 80, gate: G.check});
  const original = await atGate(reset, G.check);
  await action(reset, A.arm, 400);
  await action(reset, A.timer, 0, original.timer, true);
  await gate(reset);
  reset.control = await hold([reset], 170);
  result = await finish(reset, R.timer);
  require(result.timer !== original.timer && result.stale >= 1, 'old timer generation was reused');

  const disarmed = await start('disarm-and-reject-late-timer', {timerMs: 60, gate: G.check});
  const old = await atGate(disarmed, G.check);
  await action(disarmed, A.disarm);
  await gate(disarmed);
  disarmed.control = await hold([disarmed], 150);
  await action(disarmed, A.timer, 0, old.timer, true);
  await action(disarmed, A.notify);
  result = await finish(disarmed, R.notify);
  require(!result.timerArmed && result.lastBudgetMs === -1, 'disarmed deadline still bounded the wait');

  const expiredReset = await start('reset-after-futex-timeout', {timerMs: 60, gate: G.timeout});
  await atGate(expiredReset, G.timeout);
  await action(expiredReset, A.arm, 100);
  await gate(expiredReset);
  result = await finish(expiredReset, R.timer);
  require(result.timedOut >= 2, 'reset did not wait for the new deadline');

  const expiredDisarm = await start('disarm-after-futex-timeout', {timerMs: 60, gate: G.timeout});
  await atGate(expiredDisarm, G.timeout);
  await action(expiredDisarm, A.disarm);
  await gate(expiredDisarm);
  expiredDisarm.control = await hold([expiredDisarm], 100);
  await action(expiredDisarm, A.close);
  await finish(expiredDisarm, R.close);

  const expiredCancel = await start('cancel-before-expired-timer-commits', {timerMs: 60, gate: G.timeout});
  await atGate(expiredCancel, G.timeout);
  await action(expiredCancel, A.cancel);
  await gate(expiredCancel);
  await finish(expiredCancel, R.cancel);

  const concurrent = await Promise.all([0, 1, 2].map((slot) =>
    start(`cancel-routing-${slot}`, {slot, timerMs: slot === 1 ? 5000 : -1})));
  for (const c of concurrent) await parked(c);
  report.concurrentControl = await hold(concurrent, 200);
  await action(concurrent[1], A.cancel);
  await finish(concurrent[1], R.cancel);
  await hold([concurrent[0], concurrent[2]], 80);
  await action(concurrent[0], A.close);
  await finish(concurrent[0], R.close);
  await action(concurrent[2], A.notify, 789);
  await finish(concurrent[2], R.notify);

  const reused = await start('late-events-after-request-reuse', {slot: 1, gate: G.start});
  await atGate(reused, G.start);
  for (const a of [A.cancel, A.close, A.notify, A.timer]) {
    const late = await rpc('action', {...concurrent[1], action: a, timer: concurrent[1].initial.timer});
    require(late.rc === report.clocks.errors.stale, 'an old request event reached the reused slot');
  }
  await gate(reused);
  await hold([reused], 80);
  await action(reused, A.notify, 456);
  result = await finish(reused, R.notify);
  require(result.stale === 4 && result.payload === 456, 'stale events contaminated the next request');

  let random = seed >>> 0;
  function next() { random ^= random << 13; random ^= random >>> 17; random ^= random << 5; return random >>> 0; }
  for (let iteration = 0; iteration < 64; iteration++) {
    const c = await start(`completion-cancel-race-${iteration}`);
    await parked(c);
    const actions = next() & 1 ? [A.notify, A.cancel] : [A.cancel, A.notify];
    const gapMs = next() & 1;
    c.stimulus = {actions, gapMs};
    c.race = await rpc('race', {...c, actions, gapMs, value: iteration});
    result = await finish(c, [R.notify, R.cancel]);
    const cancelResult = c.race.actions.find((a) => a.action === A.cancel);
    require(c.race.actions.every((a) => a.rc >= 0 || a.rc === report.clocks.errors.stale), 'race event returned an unexpected error');
    require(result.outcome !== R.notify || cancelResult.rc === report.clocks.errors.stale, 'accepted cancellation was lost before completion');
    report.raceOutcomes[result.outcome === R.notify ? 'notified' : 'cancelled']++;
  }
  require(!fatal, 'wait Worker reported a late error');
  for (const c of report.cases) {
    require(c.status === 'completed', `${c.name}: case did not complete`);
    c.status = 'passed';
  }
  report.status = 'passed';
} catch (error) {
  report.error = String(error.stack || error);
} finally {
  clearTimeout(readyTimeout);
  worker?.terminate();
  window.probeResult = report;
  document.querySelector('#status').textContent = JSON.stringify(report);
}
