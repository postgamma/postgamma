/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
import {Database, ClosedError, DatabaseError} from '../src/api/database.js';
import {assert, scalar} from './helpers.js';
import {waitLock} from './concurrency.js';
import {Op, Reader, Writer} from '../src/protocol/wire.js';
import type {Handle} from '../src/protocol/wire.js';

function fatalStage(phase: string, db?: Database, iteration?: number) {
  const event = {kind: 'fatal-stage', phase, iteration, at: performance.now(), sample: db?.sample()};
  (window as unknown as {probeEvents: unknown[]}).probeEvents.push(event);
  console.debug(JSON.stringify(event));
}

function recoveryFinished(scenario: string, started: number) {
  const elapsedMs = performance.now() - started;
  (window as unknown as {probeEvents: unknown[]}).probeEvents.push({kind: 'recovery-timing', scenario, elapsedMs});
  assert(elapsedMs < 30000, 'Failure cleanup and recovery complete within 30 seconds');
}

export async function closeAuditFailure(checks: string[]) {
  const name = 'close-audit-' + crypto.randomUUID();
  const db = await Database.open({storage: {type: 'opfs', name}});
  await (await db.connect()).query('CREATE TABLE close_audit(value int)');
  const transport = (db as unknown as {transport: {
    call(op: number, handle?: Handle, write?: (w: Writer) => void): Promise<Reader>;
  }}).transport;
  const call = transport.call.bind(transport);
  let closes = 0, stops = 0;
  // Change only the audit reply after the real kernel frees the instance.
  // STOP still joins actual threads and releases the OPFS ownership lock.
  transport.call = async (op, handle, write) => {
    const reply = await call(op, handle, write);
    if (op === Op.stop) stops++;
    if (op !== Op.close) return reply;
    closes++;
    for (let i = 0; i < 4; i++) assert(reply.u32() === 0, 'Native close has no real resource leaks');
    reply.end();
    return new Reader(new Writer().u32(1).u32(0).u32(0).u32(0).finish());
  };
  const started = performance.now(), closed = db.close();
  const error = await closed.then(() => null, error => error);
  assert(error instanceof Error && error.message === 'Native resources remain after close', 'Audit error reaches the caller');
  assert(db.close() === closed && closes === 1 && stops === 1, 'Audit failure never retries the retired native handle');
  const reopened = await Database.open({storage: {type: 'opfs', name}, create: false});
  assert(scalar(await (await reopened.connect()).query('SELECT count(*) FROM close_audit')) === '0', 'OPFS reopens immediately after failed audit');
  recoveryFinished('close-audit', started);
  const metrics = await reopened.close();
  const origin = await navigator.storage.getDirectory();
  await (await origin.getDirectoryHandle('postgamma')).removeEntry(name, {recursive: true});
  checks.push('close-audit-cleanup', 'close-audit-opfs-reopen');
  return metrics;
}

export async function failedOpenRecovery(checks: string[]) {
  const name = 'open-failed-' + crypto.randomUUID();
  const options = {storage: {type: 'opfs' as const, name}};
  const started = performance.now();
  const failure = await Database.open({...options, create: false}).then(() => null, error => error);
  assert(failure instanceof DatabaseError, 'Missing database fails after acquiring its storage');
  recoveryFinished('open-failed', started);
  const created = await Database.open(options);
  assert(scalar(await (await created.connect()).query('SELECT 17')) === '17', 'Failed OPEN releases storage and ownership for creation');
  const metrics = await created.close();
  const origin = await navigator.storage.getDirectory();
  await (await origin.getDirectoryHandle('postgamma')).removeEntry(name, {recursive: true});
  checks.push('failed-open-opfs-recreate');
  return metrics;
}

export async function lifecycle(db: Database, checks: string[]) {
  await new Promise(resolve => setTimeout(resolve, 1000));
  await db.telemetry();
  const baseline = db.sample()!, begin = performance.now();
  await new Promise(resolve => setTimeout(resolve, 2000));
  await db.telemetry();
  const observed = db.sample()!, elapsedMs = performance.now() - begin;
  const driverWakes = (observed.driverWakeCount - baseline.driverWakeCount) >>> 0;
  assert(observed.periodicSamples === 0, 'Default control Worker has no periodic sampler');
  assert(driverWakes / (elapsedMs / 1000) < 100, 'Idle host driver waits for notifications');
  (window as unknown as {probeEvents: unknown[]}).probeEvents.push({kind: 'idle-control',
    elapsedMs, driverWakes, periodicSamples: observed.periodicSamples, visibility: document.visibilityState,
    scope: 'host driver wakeups only; browser CPU and power are not measured'});
  checks.push('idle-notification-wait', 'sampling-disabled-by-default');
  await closeProgress(db, checks);
  const a = await db.connect('close-holder'), b = await db.connect('close-waiter'), c = await db.connect('close-observer');
  await a.query('CREATE TABLE close_test(id int PRIMARY KEY)'); await a.query('INSERT INTO close_test VALUES (1)');
  await a.query('BEGIN'); await a.query('SELECT * FROM close_test FOR UPDATE');
  let settlements = 0;
  const blocked = b.query('SELECT * FROM close_test FOR UPDATE').then(
    value => {settlements++; return value;}, error => {settlements++; return error;});
  await waitLock(c, (await b.status()).pid);
  await Promise.all([b.close(), b.close()]);
  const closed = await blocked;
  assert(closed instanceof ClosedError || closed instanceof DatabaseError && closed.sqlstate === '57014', 'Blocked query is terminated by connection close');
  assert(settlements === 1 && (await a.status()).transaction === 2 && scalar(await c.query('SELECT 1')) === '1', 'Connection close leaves A/C unaffected');
  await a.query('ROLLBACK');
  const rows = await a.execute("SELECT g, repeat('q', 1024) FROM generate_series(1, 100000) g", {chunkRows: 32});
  await rows.next();
  const copy = await c.execute("COPY (SELECT g, repeat('q', 4096) FROM generate_series(1, 100000) g) TO STDOUT");
  const stream = (await copy.next())!.copy!; await stream.read();
  await new Promise(resolve => setTimeout(resolve, 100));
  const firstClose = db.close(), repeatedClose = db.close();
  assert(firstClose === repeatedClose, 'Instance close shares one completion');
  const metrics = await firstClose;
  assert(settlements === 1, 'No duplicate query settlement');
  let rejected = 0;
  for (const operation of [() => db.connect(), () => a.query('SELECT 1'), () => rows.next(), () => stream.read()]) {
    try {await operation();} catch {rejected++;}
  }
  assert(rejected === 4, 'Closed descendants reject new operations');
  checks.push('close-blocked-query', 'idempotent-close', 'close-full-result-and-copy', 'closed-descendants');
  const fresh = await Database.open({diagnostics: {sampleIntervalMs: 10}}), connection = await fresh.connect();
  assert(scalar(await connection.query('SELECT 7')) === '7', 'New module starts after full close');
  // Close while a query is being submitted, before its request handle arrives.
  const connecting = fresh.connect('close-during-connect');
  const starting = connection.execute('SELECT pg_sleep(2)').then(async request => {
    try {await request.next();} finally {await request.close();}
  }).then(() => 'done', error => error);
  await fresh.close(); await starting;
  assert((fresh.diagnostics() as {periodicSamples: number}).periodicSamples > 0, 'Explicit diagnostic sampling remains available');
  const late = await connecting;
  let lateRejected = false; try {await late.query('SELECT 1');} catch {lateRejected = true;}
  assert(lateRejected, 'Connection created during close is also retired');
  (window as unknown as {probeEvents: unknown[]}).probeEvents.push({kind: 'lifecycle', metrics,
    recreated: fresh.diagnostics(), settlements});
  checks.push('close-during-submission', 'recreate-after-close');
}

async function closeProgress(db: Database, checks: string[]) {
  const a = await db.connect('delayed-close'), b = await db.connect('close-progress'),
    c = await db.connect('close-cancel');
  const handle = (a as unknown as {handle: Handle}).handle;
  const transport = (db as unknown as {transport: {
    call(op: number, handle?: Handle, write?: (w: Writer) => void): Promise<Reader>;
  }}).transport;
  const call = transport.call.bind(transport);
  let submitted!: () => void;
  const submission = new Promise<void>(resolve => {submitted = resolve;});
  transport.call = (op, h, write) => {
    const work = call(op, h, write);
    if (op === Op.disconnect && h?.id === handle.id) submitted();
    return work;
  };
  const request = await c.execute('SELECT pg_sleep(20)');
  const pending = request.next().then(() => null, error => error);
  let closed = false, held = false, released = false, timer: ReturnType<typeof setTimeout> | undefined;
  let closing: Promise<unknown> | undefined;
  try {
    (await call(65533, handle, w => w.u32(1))).end(); held = true;
    closing = a.close().then(() => {closed = true; return null;}, error => error);
    await submission;
    await Promise.race([
      (async () => {
        assert(scalar(await b.query('SELECT 42')) === '42', 'SQL progresses while another backend reference is retained');
        assert(!closed && !released, 'Connection close remains pending while its backend reference is retained');
        await request.cancel();
        const canceled = await pending;
        assert(canceled instanceof DatabaseError && canceled.sqlstate === '57014', 'CANCEL progresses during unrelated connection close');
        await request.close();
        assert(scalar(await c.query('SELECT 43')) === '43', 'Canceled connection is reusable before delayed close completes');
      })(),
      new Promise<never>((_, reject) => {timer = setTimeout(() => reject(new Error('Connection close blocked unrelated SQL or cancellation')), 3000);}),
    ]);
    checks.push('disconnect-yields-to-sql', 'disconnect-yields-to-cancel');
  } finally {
    clearTimeout(timer);
    if (held) {(await call(65533, handle, w => w.u32(0))).end(); released = true;}
    transport.call = call;
    await closing;
    await Promise.allSettled([request.close(), a.close(), b.close(), c.close()]);
  }
  assert(closed, 'Connection close finishes after the retained backend reference is released');
  checks.push('disconnect-waits-for-backend-release');
}

export async function disconnectTimeout(db: Database, checks: string[]) {
  const a = await db.connect('close-deadline'), b = await db.connect('deadline-observer');
  const handle = (a as unknown as {handle: Handle}).handle;
  const transport = (db as unknown as {transport: {
    call(op: number, handle?: Handle, write?: (w: Writer) => void): Promise<Reader>;
  }}).transport;
  const call = transport.call.bind(transport);
  let submitted!: () => void, attempts = 0, held = false;
  const submission = new Promise<void>(resolve => {submitted = resolve;});
  transport.call = (op, h, write) => {
    const work = call(op, h, write);
    if (op === Op.disconnect && h?.id === handle.id) {attempts++; submitted();}
    return work;
  };
  try {
    (await call(65533, handle, w => w.u32(1))).end(); held = true;
    const started = performance.now();
    const closing = a.close().then(() => null, error => error);
    assert(a.close() === a.close(), 'Pending disconnect shares its completion');
    await submission;
    assert(scalar(await b.query('SELECT 44')) === '44', 'Another connection progresses before the close deadline');
    const error = await closing, elapsedMs = performance.now() - started;
    assert(error instanceof DatabaseError && error.status === 3, 'Retained backend reference causes a visible close timeout');
    assert(attempts > 1 && elapsedMs >= 9900 && elapsedMs < 15000, 'Disconnect retries share the original 10-second deadline');
    const rejected = await call(Op.status, handle).then(() => null, error => error);
    assert(rejected instanceof DatabaseError && rejected.status === 2, 'Partially closed native connection rejects ordinary operations');
    (await call(65533, handle, w => w.u32(0))).end(); held = false;
    await a.close();
    (window as unknown as {probeEvents: unknown[]}).probeEvents.push({kind: 'disconnect-timeout', attempts, elapsedMs});
    checks.push('disconnect-bounded-deadline', 'disconnect-timeout-retry', 'disconnect-rejects-new-work');
    const fresh = await db.connect('close-slot-reuse');
    try {
      const freshHandle = (fresh as unknown as {handle: Handle}).handle;
      assert(freshHandle.id === handle.id && freshHandle.generation !== handle.generation, 'Closed connection slot is reused with a fresh generation');
      const stale = await call(Op.disconnect, handle).then(() => null, error => error);
      assert(stale instanceof DatabaseError && stale.status === 1, 'Stale close cannot retire the reused connection');
      assert(scalar(await fresh.query('SELECT 45')) === '45', 'Reused connection survives a stale disconnect');
      checks.push('disconnect-stale-generation');
    } finally {await fresh.close();}
  } finally {
    if (held) (await call(65533, handle, w => w.u32(0))).end();
    transport.call = call;
    await Promise.allSettled([a.close(), b.close()]);
  }
}
export async function fatal(db: Database, checks: string[], fault: number,
                            storage?: {type: 'opfs'; name: string}, retainStorage = false) {
  fatalStage('connect-original', db);
  const a = await db.connect('fatal-a'), b = await db.connect('fatal-b');
  if (storage) {
    await a.query('CREATE TABLE IF NOT EXISTS fatal_ledger(id int PRIMARY KEY)');
    await a.query('TRUNCATE fatal_ledger');
    await a.query('INSERT INTO fatal_ledger VALUES (1)');
    await a.query('BEGIN');
    await a.query('INSERT INTO fatal_ledger VALUES (2)');
  }
  const waiters = [a.query('SELECT pg_sleep(20)'), b.query('SELECT pg_sleep(20)'),
    a.query('SELECT 1'), b.query('SELECT 2')];
  const counts = waiters.map(() => 0);
  const settled = waiters.map((work, index) => work.then(() => {counts[index]++; return 'success';}, error => {counts[index]++; return error;}));
  await db.telemetry();
  // Private test opcode is compiled only for the browser check action. It
  // traps/aborts inside the real Wasm host pthread, not a fabricated JS error.
  const transport = (db as unknown as {transport: {call(op: number, handle: undefined, write: (w: Writer) => void): Promise<unknown>}}).transport;
  const started = performance.now();
  fatalStage('inject', db);
  const injected = transport.call(65535, undefined, w => w.u32(fault)).then(() => 'success', error => error);
  const outcomes = await Promise.all([...settled, injected]);
  fatalStage('settled', db);
  assert(outcomes.every(value => value instanceof Error) && counts.every(count => count === 1), 'Fatal error settles every in-flight and queued Promise once');
  const metrics = db.diagnostics() as {terminationRequested: boolean; interruptedWorkers: number; runningWorkers: number; storageQuiesced: boolean; storageReleased: boolean};
  assert(metrics.terminationRequested && metrics.interruptedWorkers > 0 && metrics.runningWorkers === 0, 'Fatal path requests termination of all owned SDK workers');
  if (storage) assert(metrics.storageQuiesced === true && metrics.storageReleased === true,
    'Fatal path confirms I/O quiescence and physical OPFS handle release before rejecting requests');
  let rejected = false; try {await a.query('SELECT 3');} catch {rejected = true;}
  assert(rejected, 'Failed instance rejects subsequent requests');
  // Open immediately, with no sleep or retry to conceal ownership races.
  fatalStage('reopen');
  const fresh = await Database.open(storage ? {storage, create: false} : {});
  fatalStage('reopened', fresh);
  const connection = await fresh.connect();
  fatalStage('reconnected', fresh);
  assert(scalar(await connection.query('SELECT 8')) === '8', 'Fresh module runs after a fatal instance');
  fatalStage('query-complete', fresh);
  if (storage) {
    assert(scalar(await connection.query('SELECT count(*) FROM fatal_ledger WHERE id = 1')) === '1', 'Acknowledged commit survives a fatal instance');
    assert(scalar(await connection.query('SELECT count(*) FROM fatal_ledger WHERE id = 2')) === '0', 'Uncommitted transaction rolls back after fatal recovery');
    await connection.query('INSERT INTO fatal_ledger VALUES (3)');
    checks.push('opfs-fatal-immediate-reopen', 'opfs-fatal-commit-recovery', 'opfs-fatal-rollback');
  }
  recoveryFinished(storage ? 'fatal-opfs' : 'fatal', started);
  fatalStage('close-replacement', fresh);
  await fresh.close();
  fatalStage('closed-replacement', fresh);
  (window as unknown as {probeEvents: unknown[]}).probeEvents.push({kind: 'fatal', fault, metrics,
    recreated: fresh.diagnostics(), settlements: counts});
  checks.push(fault === 1 ? 'real-wasm-trap' : 'real-wasm-abort', 'fatal-settles-all', 'fatal-instance-rejected', 'recreate-after-fatal');
  if (storage && !retainStorage) {
    const origin = await navigator.storage.getDirectory();
    await (await origin.getDirectoryHandle('postgamma')).removeEntry(storage.name, {recursive: true});
  }
}

export async function repeatedFatal(checks: string[], metrics: unknown[], fault: number, repetitions: number) {
  const storage = {type: 'opfs' as const, name: 'fatal-repeated-' + crypto.randomUUID()};
  // Reuse durable data, but create a fresh module for every attempt. The error
  // path still opens its replacement immediately, without sleeps or retries.
  for (let iteration = 1; iteration <= repetitions; iteration++) {
    fatalStage('open-original', undefined, iteration);
    const db = await Database.open({storage, create: iteration === 1});
    fatalStage('opened-original', db, iteration);
    await fatal(db, checks, fault, storage, true);
    metrics.push(db.diagnostics());
    (window as unknown as {probeEvents: unknown[]}).probeEvents.push({kind: 'fatal-iteration', fault, iteration});
  }
  const origin = await navigator.storage.getDirectory();
  await (await origin.getDirectoryHandle('postgamma')).removeEntry(storage.name, {recursive: true});
}
