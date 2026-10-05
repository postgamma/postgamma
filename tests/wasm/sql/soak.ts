/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
import {Database, DatabaseError} from '../src/api/database.js';
import type {Connection} from '../src/api/database.js';
import {Writer} from '../src/protocol/wire.js';
import {assert, scalar} from './helpers.js';

type Command = {lane: number; sql: string; started: number; elapsedMs?: number; sqlstate?: string | null; copyData?: string};
const page = window as unknown as {soakEvent(event: unknown): Promise<void>; soakState: unknown};
const emit = (value: unknown) => page.soakEvent(value);

export async function soak(checks: string[], metrics: unknown[]) {
  const params = new URL(location.href).searchParams;
  const seconds = Number(params.get('seconds') ?? 120), seed = Number(params.get('seed') ?? 1);
  const rounds = Number(params.get('rounds') ?? 0), reopenEvery = Number(params.get('reopen') ?? 50);
  assert(Number.isSafeInteger(seed) && seed > 0 && seed <= 0xffffffff, 'Valid uint32 seed');
  assert(Number.isInteger(seconds) && seconds >= 1 && seconds <= 28800, 'Valid duration');
  assert(Number.isInteger(rounds) && rounds >= 0 && rounds <= 1000000, 'Valid replay round count');
  assert(Number.isInteger(reopenEvery) && reopenEvery >= 2, 'Valid reopen interval');
  let randomState = seed;
  const random = () => {randomState ^= randomState << 13; randomState ^= randomState >>> 17; randomState ^= randomState << 5; return randomState >>> 0;};
  const storage = {type: 'opfs' as const, name: 'soak-' + crypto.randomUUID()};
  let db: Database, connections: Connection[] = [], unsubscribe: (() => void) | undefined;
  let notifications: string[] = [], commands: Command[] = [];
  let notificationFailure: unknown;
  const model = new Map<number, number>(Array.from({length: 64}, (_, id) => [id, 0]));
  let copyModel: string[][] = [];
  let iteration = 0, cycles = 0, cancellations = 0, crashes = 0, orderly = 0;
  let totalCommands = 0, totalCopyBytes = 0;
  let started = performance.now();
  // Diagnostic runs invoke this only while soakEvent holds the workload at a
  // verified boundary. Collect every backend: one session cannot inspect
  // another session's pg_backend_memory_contexts.
  Object.assign(window, {soakMemoryContexts: async () => Promise.all(connections.map(async connection => {
    const results = await connection.exec(`SELECT name, sum(total_bytes)::text AS total_bytes,
      sum(used_bytes)::text AS used_bytes FROM pg_backend_memory_contexts
      GROUP BY name ORDER BY name`);
    return results[0].rows;
  }))});
  async function sql(lane: number, text: string) {
    const row: Command = {lane, sql: text, started: performance.now()}; commands.push(row); totalCommands++;
    try {return await connections[lane].exec(text);}
    catch (error) {if (error instanceof DatabaseError) row.sqlstate = error.sqlstate; throw error;}
    finally {row.elapsedMs = performance.now() - row.started;}
  }
  async function connect(create: boolean) {
    db = await Database.open({storage, create, diagnostics: {sampleIntervalMs: 1000}});
    connections = await Promise.all(Array.from({length: 4}, (_, lane) => db.connect('soak/' + lane)));
    if (create) await sql(0, `CREATE EXTENSION pg_trgm; CREATE EXTENSION hstore;
      CREATE TABLE soak_rows(id int PRIMARY KEY, value int NOT NULL);
      INSERT INTO soak_rows SELECT i,0 FROM generate_series(0,63) i;
      CREATE TABLE soak_copy(id int PRIMARY KEY, payload text);
      CREATE TABLE soak_ext(id int PRIMARY KEY, value text, attrs hstore);
      INSERT INTO soak_ext SELECT i,'row-'||i,hstore('id',i::text) FROM generate_series(1,64) i;
      CREATE INDEX soak_trgm ON soak_ext USING gin(value gin_trgm_ops);
      CREATE INDEX soak_hstore ON soak_ext USING gin(attrs);`);
    notifications = [];
    notificationFailure = undefined;
    await sql(3, 'LISTEN soak_notice');
    unsubscribe = connections[3].onNotification(value => {notifications.push(value.payload);}, error => {notificationFailure = error;});
  }
  async function verify() {
    const actual = (await sql(0, 'SELECT id,value FROM soak_rows ORDER BY id'))[0].rows;
    const expected = [...model].map(([id, value]) => [String(id), String(value)]);
    assert(JSON.stringify(actual) === JSON.stringify(expected), 'Acknowledged model matches every durable row');
    assert(JSON.stringify((await sql(0, 'SELECT id,payload FROM soak_copy ORDER BY id'))[0].rows) === JSON.stringify(copyModel), 'COPY commit/rollback matches every row');
    assert(scalar(await sql(0, "SELECT to_regclass('soak_transient')::text")) === null, 'Rolled-back DDL leaves no relation');
    assert(scalar(await sql(0, "SELECT count(*) FROM soak_ext WHERE attrs @> 'id=>7'::hstore AND value % 'row-7'")) === '1', 'Extension indexes remain usable');
    const deadline = performance.now() + 2000;
    let telemetry = await db.telemetry();
    while ((telemetry.requests || telemetry.running) && performance.now() < deadline) {
      await new Promise(resolve => setTimeout(resolve, 1)); telemetry = await db.telemetry();
    }
    assert(telemetry.connections === 4n && telemetry.requests === 0n && telemetry.running === 0n, 'Quiescent sessions have no leaked requests');
  }
  async function close() {
    const closed = await db.close() as {returnedWorkers: number; terminationRequestedWorkers: number};
    assert(closed.returnedWorkers > 0 && closed.terminationRequestedWorkers === closed.returnedWorkers,
      'Normal close requests termination of every returned pthread; the browser gate checks physical reclamation');
    metrics.push(closed);
  }
  async function reopen(fault: number) {
    await verify(); unsubscribe?.();
    if (fault) {
      await sql(0, "BEGIN; UPDATE soak_rows SET value=-1 WHERE id IN (0,1); CREATE TABLE soak_transient(x int); NOTIFY soak_notice,'uncommitted'");
      await emit({kind: 'fault', iteration, fault, commands}); commands = [];
      const transport = (db as unknown as {transport: {call(op: number, h: undefined, write: (w: Writer) => void): Promise<unknown>}}).transport;
      await transport.call(65535, undefined, w => w.u32(fault)).then(() => {throw new Error('Fault did not discard the module');}, () => {});
      const closed = db.diagnostics() as {storageReleased?: boolean; runningWorkers?: number; crashedWorkerRetired?: boolean};
      assert(closed.storageReleased && closed.runningWorkers === 0, 'Fatal teardown releases OPFS and callers');
      assert(closed.crashedWorkerRetired === true, 'Failed native thread confirms its own scope retirement');
      metrics.push(closed); crashes++;
    } else {await close(); orderly++;}
    const before = performance.now(); await connect(false); await verify();
    cycles++;
    await emit({kind: 'reopen', iteration, fault, elapsedMs: performance.now() - before,
      sample: db.sample(), metrics: metrics.at(-1), model: [...model], cycles});
    while (metrics.length > 3) metrics.shift(); // Full receipts were streamed to the host.
    commands = [];
  }
  await connect(true); await verify();
  await emit({kind: 'ready', seed, seconds, rounds, reopenEvery, storage: storage.name}); commands = [];
  started = performance.now();
  while (rounds ? iteration < rounds : performance.now() - started < seconds * 1000) {
    iteration++;
    const id = random() % 64, value = random() % 1000000, tag = `${seed}:${iteration}:${value}`;
    const choice = random() % 4, operation = iteration === 1 ? 1 : choice;
    page.soakState = {iteration, seed, randomState, id, value, operation, phase: 'mixed', commands};
    await Promise.all([
      (async () => {await sql(0, `BEGIN; UPDATE soak_rows SET value=${value} WHERE id=${id}; NOTIFY soak_notice,'${tag}'; COMMIT`); model.set(id, value);})(),
      (async () => {
        await sql(1, 'BEGIN; CREATE TABLE soak_transient(x int); SAVEPOINT recover');
        try {await sql(1, 'SELECT 1/0'); throw new Error('Expected SQL error');}
        catch (error) {assert(error instanceof DatabaseError && error.sqlstate === '22012', 'SQL error is recoverable');}
        await sql(1, 'ROLLBACK TO SAVEPOINT recover; INSERT INTO soak_transient VALUES(1); ROLLBACK');
      })(),
      (async () => {
        await sql(2, 'BEGIN; TRUNCATE soak_copy');
        const request = await connections[2].execute('COPY soak_copy FROM STDIN');
        const result = await request.next(); assert(result?.copy, 'COPY input started');
        const rows = Array.from({length: 16}, (_, i) => `${i}\t${tag}-${i}\n`).join('');
        commands.push({lane: 2, sql: 'COPY soak_copy FROM STDIN', started: performance.now(), copyData: rows});
        totalCommands++;
        const bytes = new TextEncoder().encode(rows); totalCopyBytes += bytes.length;
        await result.copy.write(bytes); await result.copy.finish(); await result.copy.close();
        while (await request.next()) {} await request.close();
        await sql(2, operation === 0 ? 'ROLLBACK' : 'COMMIT');
        if (operation !== 0) copyModel = Array.from({length: 16}, (_, i) => [String(i), `${tag}-${i}`]);
      })(),
      (async () => {
        if (operation === 1) {
          const request = await connections[3].execute('SELECT pg_sleep(30)');
          commands.push({lane: 3, sql: 'SELECT pg_sleep(30) /* cancelled after PgSleep */', started: performance.now()});
          totalCommands++;
          const waiting = request.next().then(value => value, error => error);
          const pid = (await connections[3].status()).pid, deadline = performance.now() + 10000;
          let running = false;
          while (!running && performance.now() < deadline)
            running = scalar(await sql(0, `SELECT EXISTS(SELECT FROM pg_stat_activity WHERE pid=${pid} AND wait_event='PgSleep')`)) === 't';
          assert(running, 'Cancellation reaches an executing query');
          await request.cancel(); const error = await waiting;
          assert(error instanceof DatabaseError && error.sqlstate === '57014', 'Cancellation preserves SQLSTATE');
          await request.close(); cancellations++;
        } else await sql(3, "SELECT attrs->'id' FROM soak_ext WHERE value % 'row-7' ORDER BY id");
      })(),
    ]);
    const deadline = performance.now() + 5000;
    while (!notifications.includes(tag) && performance.now() < deadline) await new Promise(resolve => setTimeout(resolve, 1));
    if (notificationFailure) throw notificationFailure;
    assert(notifications.length === 1 && notifications[0] === tag, 'Exactly one committed notification, without a listener SQL kick');
    notifications = [];
    await verify();
    if (iteration % 20 === 0) {await sql(0, 'VACUUM'); await sql(0, 'CHECKPOINT');}
    page.soakState = {iteration, seed, randomState, phase: 'verified', commands};
    await emit({kind: 'round', iteration, randomState, id, value, operation, commands,
      elapsedMs: performance.now() - started, sample: db.sample(), cancellations, totalCommands, totalCopyBytes});
    commands = [];
    if (iteration % reopenEvery === 0) await reopen(cycles % 3 === 0 ? 0 : cycles % 3 === 1 ? 1 : 2);
  }
  // Every invocation covers both clean reopen and crash recovery, even a
  // short deterministic replay that finishes before its first scheduled cycle.
  if (!orderly) await reopen(0);
  if (!crashes) await reopen(1);
  await verify(); unsubscribe?.(); await close();
  await emit({kind: 'complete', seed, iteration, cycles, crashes, orderly, cancellations,
    elapsedMs: performance.now() - started, totalCommands, totalCopyBytes, model: [...model]});
  checks.push('soak-four-connections', 'soak-commit-model', 'soak-ddl-savepoint-rollback',
    'soak-copy', 'soak-notification-barrier', 'soak-sql-error-reuse', 'soak-clean-reopen', 'soak-crash-recovery');
  if (cancellations) checks.push('soak-cancel-reuse');
}
