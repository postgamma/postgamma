/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
import {Database, DatabaseError} from '../src/api/database.js';
import type {Connection} from '../src/api/database.js';
import type {Handle} from '../src/protocol/wire.js';
import {Op, Reader, Writer} from '../src/protocol/wire.js';
import {assert, scalar} from './helpers.js';

export async function connectWatchdog(checks: string[], metrics: unknown[]) {
  const storage = {type: 'opfs' as const, name: 'connect-deadline-' + crypto.randomUUID()};
  const db = await Database.open({storage});
  const seed = await db.connect();
  await seed.exec('CREATE TABLE startup_committed(value int); INSERT INTO startup_committed VALUES(73)');
  await seed.close();
  const transport = (db as unknown as {transport: {call(op: number, h: undefined, write: (w: Writer) => void): Promise<Reader>}}).transport;
  (await transport.call(65532, undefined, w => w.u32(1))).end();
  // Keep a real ticket owned by startup even after its native deadline. The
  // API must discard the instance, not free a still-owned native payload.
  const started = performance.now();
  const failure = await db.connect('unretired').then(() => null, error => error);
  const elapsedMs = performance.now() - started;
  assert(failure instanceof Error && failure.message.includes('90s deadline'),
    'Unretired startups reject through the bounded instance discard');
  assert(elapsedMs >= 89000 && elapsedMs < 105000, 'Startup discard and cleanup have a bounded deadline');
  const discarded = db.diagnostics() as {storageReleased?: boolean; runningWorkers?: number};
  assert(discarded.storageReleased === true && discarded.runningWorkers === 0,
    'Startup rejection waits for OPFS release and native caller termination');
  checks.push('connect-bounded-startup-discard', 'connect-discard-opfs-release');
  const recovered = await Database.open({storage, create: false});
  try {
    const connection = await recovered.connect();
    assert(scalar(await connection.query('SELECT value FROM startup_committed')) === '73',
      'Discarded startup preserves prior committed data and permits immediate reopen');
    await connection.close();
    checks.push('connect-discard-recovery');
  } finally {metrics.push(await recovered.close());}
  (window as unknown as {probeEvents: unknown[]}).probeEvents.push({kind: 'connect-startup-discard', elapsedMs});
}

async function controlHarness(db: Database, checks: string[]) {
  const transport = (db as unknown as {transport: {
    call(op: number, handle?: Handle, write?: (w: Writer) => void): Promise<Reader>;
  }}).transport;
  const gate = async (action: number) => {
    const reply = await transport.call(65532, undefined, w => w.u32(action));
    const selected = action === 2 ? reply.u32() : 0; reply.end(); return selected;
  };
  const observer = await db.connect('control-observer');
  async function gated(name: string, start: () => Promise<unknown>, inspect?: () => Promise<void>) {
    await gate(1);
    let settled = false, timer: ReturnType<typeof setTimeout> | undefined;
    const work = start().then(value => {settled = true; return {value};}, error => {settled = true; return {error};});
    try {
      await Promise.race([
        (async () => {
          while (!await gate(2)) await Promise.resolve();
          assert(scalar(await observer.query('SELECT 61')) === '61', name + ': unrelated SQL progresses');
          if (!inspect) {
            const probe = await observer.execute('SELECT pg_sleep(20)');
            const result = probe.next().then(() => null, error => error);
            await probe.cancel();
            const canceled = await result;
            assert(canceled instanceof DatabaseError && canceled.sqlstate === '57014', name + ': unrelated cancel progresses');
            await probe.close();
            checks.push(name + '-yields-to-cancel');
          }
          await inspect?.();
          assert(!settled, name + ': lifecycle retains its pending supervisor ticket');
        })(),
        new Promise<never>((_, reject) => {timer = setTimeout(() => reject(new Error(name + ': lifecycle blocked driver')), 3000);}),
      ]);
    } finally {
      clearTimeout(timer);
      await gate(0);
      await work;
    }
    const outcome = await work;
    if ('error' in outcome) throw outcome.error;
    checks.push(name + '-yields-to-sql');
    return outcome.value;
  }
  return {observer, gated, gate, transport};
}

export async function controlTimeout(db: Database, checks: string[]) {
  const {observer, gate, transport} = await controlHarness(db, checks);
  const owner = await db.connect();
  try {
    const request = await owner.execute('SELECT pg_sleep(20)');
    const handle = (request as unknown as {handle: Handle}).handle;
    await gate(1);
    const started = performance.now();
    const closing = request.close().then(() => null, error => error);
    while (!await gate(2)) await Promise.resolve();
    assert(scalar(await observer.query('SELECT 67')) === '67', 'Pending release yields to SQL');
    const error = await closing, elapsedMs = performance.now() - started;
    assert(error instanceof DatabaseError && error.status === 3, 'Release reports bounded timeout');
    assert(elapsedMs >= 9900 && elapsedMs < 15000, 'Release shares one deadline across polls');
    (window as unknown as {probeEvents: unknown[]}).probeEvents.push({kind: 'control-timeout', elapsedMs});
    await gate(0);
    await request.close();
    const fresh = await owner.execute('SELECT 68');
    const stale = await transport.call(Op.cancel, handle).then(() => null, error => error);
    assert(stale instanceof DatabaseError && stale.status === 1, 'Retired request generation is rejected');
    assert(scalar([(await fresh.next())!]) === '68', 'Stale cancel does not reach reused slot');
    await fresh.next(); await fresh.close();
    checks.push('control-bounded-deadline', 'control-timeout-retry', 'control-stale-generation');
  } finally {await gate(0); await owner.close(); await observer.close();}
}

export async function connectReservation(db: Database, checks: string[]) {
  const {observer, gated} = await controlHarness(db, checks);
  const idle = await db.connect();
  try {
    await observer.query('BEGIN');
    const connection = await gated('connect-reservation', () => db.connect(), async () => {
      const unavailable = await db.connect().then(() => null, error => error);
      assert(unavailable instanceof DatabaseError && unavailable.status === 2, 'Startup reserves the last carrier');
      const query = await idle.query('SELECT 1').then(() => null, error => error);
      assert(query instanceof DatabaseError && query.status === 2, 'Idle connection cannot consume reserved carrier');
      await observer.query('COMMIT');
    }) as Connection;
    assert(scalar(await connection.query('SELECT 69')) === '69', 'Reserved startup completes');
    await connection.close();
    checks.push('connect-carrier-reservation', 'connect-pinned-commit');
  } finally {await idle.close(); await observer.close();}
}

export async function controlProgress(db: Database, checks: string[]) {
  const {observer, gated} = await controlHarness(db, checks);
  try {
    const a = await db.connect('control-owner');
    try {
      const request = await a.execute('SELECT pg_sleep(20)');
      const pending = request.next().then(() => null, error => error);
      await gated('cancel', () => request.cancel());
      const canceled = await pending;
      assert(canceled instanceof DatabaseError && canceled.sqlstate === '57014', 'Cancel still delivers SQLSTATE');
      await request.close();
      for (const sql of ['SELECT pg_sleep(20)',
        'COPY (SELECT g FROM generate_series(1, 1000000) g) TO STDOUT',
        'COPY (SELECT 1) TO STDOUT']) {
        const abandoned = await a.execute(sql);
        await gated('release', () => abandoned.close());
        assert(scalar(await a.query('SELECT 62')) === '62', 'Request release leaves connection reusable');
      }
      for (const operation of ['copy-close', 'copy-abort']) {
        const request = await a.execute('COPY (SELECT g FROM generate_series(1, 1000000) g) TO STDOUT');
        const stream = (await request.next())!.copy!;
        await gated(operation, () => operation === 'copy-close' ? stream.close() : stream.abort());
        await stream.close(); await request.close();
        assert(scalar(await a.query('SELECT 63')) === '63', 'COPY retirement leaves connection reusable');
      }
      await a.query('CREATE TABLE closed_copy(value int)');
      const unclaimed = await a.execute('COPY closed_copy FROM STDIN');
      await gated('unclaimed-copy-in', () => unclaimed.close());
      const input = await a.execute('COPY closed_copy FROM STDIN');
      const stream = (await input.next())!.copy!;
      await stream.write(new TextEncoder().encode('1\n'));
      await stream.close(); await input.close();
      assert(scalar(await a.query('SELECT count(*) FROM closed_copy')) === '0', 'Incomplete COPY IN is rolled back');
      checks.push('copy-in-close', 'unclaimed-copy-release');
    } finally {await a.close();}
  } finally {await observer.close();}
}

export async function connectProgress(db: Database, checks: string[]) {
  const {observer, gated, gate, transport} = await controlHarness(db, checks);
  const instance = (db as unknown as {handle: Handle}).handle;
  const configure = async (action: number, handle: Handle, capacity = 0) =>
    (await transport.call(65531, undefined, w => w.u32(action).handle(handle).u32(capacity))).end();
  try {
    // A 32-byte transport forces the startup packet through many bounded writes.
    await configure(1, instance, 32);
    const connection = await gated('connect', () => db.connect('fragmented-' + 'x'.repeat(4096))) as Connection;
    console.log('connect-progress: fragmented handshake completed');
    assert(scalar(await connection.query('SELECT 64')) === '64', 'Fragmented startup connection is usable');
    await connection.close();
    console.log('connect-progress: fragmented connection closed');
    checks.push('connect-startup-backpressure');
    const rejected = await db.connect('x'.repeat(12000)).then(() => null, error => error);
    console.log('connect-progress: rejected startup retired', rejected instanceof Error ? rejected.message : rejected);
    assert(rejected instanceof DatabaseError, 'Oversized startup packet fails normally');
    assert((await db.telemetry()).connections === 1n, 'Failed startup releases its reservation');
    checks.push('connect-failure-retirement');
    await gate(1);
    const begin = await transport.call(Op.connect, instance, w => w.text('expired-startup'));
    const pending = begin.handle(); begin.end();
    try {
      const first = await transport.call(Op.connectPoll, pending);
      assert(first.u32() === 1, 'Startup waits for its supervisor ticket'); first.end();
      await configure(2, pending);
      const expired = await transport.call(Op.connectPoll, pending);
      assert(expired.u32() === 1, 'Expired startup retains its outstanding ticket'); expired.end();
      assert(scalar(await observer.query('SELECT 65')) === '65', 'Timeout retirement yields to another connection');
      await gate(0);
      for (;;) {
        try {
          const reply = await transport.call(Op.connectPoll, pending);
          assert(reply.u32() === 1, 'Expired startup cannot succeed'); reply.end();
        } catch (error) {
          assert(error instanceof DatabaseError && error.status === 3, 'Startup reports timeout after safe retirement');
          break;
        }
      }
      const stale = await transport.call(Op.connectPoll, pending).then(() => null, error => error);
      assert(stale instanceof DatabaseError && stale.status === 1, 'Retired startup handle is rejected');
      assert((await db.telemetry()).connections === 1n, 'Expired startup releases its reservation');
    } finally {await gate(0);}
    const fresh = await db.connect('after-expiry');
    assert(scalar(await fresh.query('SELECT 66')) === '66', 'Fresh connection succeeds after startup timeout');
    await fresh.close();
    checks.push('connect-deadline-retirement', 'connect-stale-generation');
  } finally {
    await configure(1, instance, 65536);
    await observer.close();
  }
}
