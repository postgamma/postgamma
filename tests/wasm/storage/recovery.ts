/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
import {Database} from '../src/api/database.js';
import type {Connection} from '../src/api/database.js';
import {scalar, assert} from '../tests/helpers.js';
import {Control} from './control.js';
import {storageBudget} from './budget.js';

const page = window as any;
page.probeEvents = []; page.recovery = undefined;
const NativeWorker = window.Worker;
let ownerWorker: Worker;
window.Worker = class extends NativeWorker {
  constructor(url: string | URL, options?: WorkerOptions) {
    super(url, options); if (String(url).endsWith('/worker/sql.js')) ownerWorker = this;
  }
};
let db: Database, connection: Connection, control: Control;
let pending: Promise<unknown> | undefined, acknowledged = false, ticks = 0;
setInterval(() => ticks++, 1);
async function probe(phase: number) {
  const r = await (db as any).transport.call(65534, undefined, (w: any) => w.u32(phase));
  const checks = r.u32(); r.end(); assert(checks > 0, 'Recovery file identity probe'); return checks;
}
function options(name: string, create: boolean) {
  const channel = crypto.randomUUID(); control = new Control(channel);
  return {create, storage: {type: 'opfs' as const, name}, storageTestChannel: channel};
}
async function open(name: string, create = false) {
  page.probeEvents.push({kind: 'open-begin', name, create});
  db = await Database.open(options(name, create));
  page.probeEvents.push({kind: 'instance-opened', name});
  connection = await db.connect('recovery');
  page.probeEvents.push({kind: 'connected', name});
  const mount = await control.wait(e => e.mounted);
  return {mount, ticks};
}
async function initialize(name: string) {
  await open(name, true);
  await connection.query('CREATE TABLE ledger(id text, part int, PRIMARY KEY(id, part))');
  await connection.query("INSERT INTO ledger VALUES('baseline',1),('baseline',2)");
  await connection.query('CREATE TABLE payload(id int PRIMARY KEY, value text)');
  await connection.query('ALTER TABLE payload ALTER COLUMN value SET STORAGE PLAIN');
  await connection.query("INSERT INTO payload SELECT g, repeat('a', 4000) FROM generate_series(1,200) g");
  await connection.query('CHECKPOINT');
  return {acknowledged: ['baseline'], isolated: crossOriginIsolated};
}
async function arm(id: string, boundary: string) {
  assert(/^[a-z0-9_]+$/.test(id), 'Test case identifier');
  await connection.query(`INSERT INTO ledger VALUES('${id}_ack',1),('${id}_ack',2)`);
  acknowledged = false;
  let operation: string, path: string, action = 'pause', work: () => Promise<unknown>;
  if (boundary.startsWith('wal-')) {
    await connection.query('BEGIN');
    await connection.query(`INSERT INTO ledger VALUES('${id}_pending',1),('${id}_pending',2)`);
    operation = boundary === 'wal-before' ? 'flush-before' : 'flush-after'; path = '/pg_wal/';
    work = () => connection.query('COMMIT');
  } else if (boundary.startsWith('compact-')) {
    operation = 'checkpoint-' + boundary.slice('compact-'.length);
    path = 'snapshot';
    work = () => control.checkpoint();
  } else if (boundary === 'checkpoint') {
    await connection.query("UPDATE payload SET value = repeat('b',4000)");
    operation = 'write'; path = '/' + scalar(await connection.query("SELECT pg_relation_filepath('payload')"));
    work = () => connection.query('CHECKPOINT');
  } else if (boundary === 'extension') {
    await connection.query(`CREATE TABLE extension_${id}(value text)`);
    await connection.query(`ALTER TABLE extension_${id} ALTER COLUMN value SET STORAGE PLAIN`);
    operation = 'write'; path = '/' + scalar(await connection.query(`SELECT pg_relation_filepath('extension_${id}')`));
    await connection.query('BEGIN');
    work = () => connection.query(`INSERT INTO extension_${id} SELECT repeat('x',4000) FROM generate_series(1,200) g`);
  } else if (boundary === 'create') {
    await connection.query('BEGIN'); operation = 'create'; path = '/base/';
    work = () => connection.query(`CREATE TABLE created_${id}(id int)`);
  } else {
    await probe(12); path = 'move';
    operation = boundary === 'rename-written' ? 'namespace-written' : 'namespace-flushed';
    if (boundary === 'journal-tail') {operation = 'namespace-before'; action = 'partial';}
    work = () => probe(13);
  }
  await control.arm(operation, path, action);
  pending = work().then(() => {acknowledged = true;}, error => ({error: String(error)}));
  const gate = await control.wait(e => e.gate === (boundary === 'journal-tail' ? 'namespace-partial' : operation), 15000);
  assert(!acknowledged, 'Gated operation has not been acknowledged');
  return {gate, acknowledged, ticks, sample: db.sample()};
}
async function verify(ids: string[], id: string, boundary: string) {
  const rows = (await connection.query('SELECT id, count(*) FROM ledger GROUP BY id ORDER BY id'))[0].rows;
  const counts = new Map(rows.map(row => [row[0], row[1]]));
  for (const ack of ['baseline', ...ids.map(value => value + '_ack')]) assert(counts.get(ack) === '2', 'Acknowledged pair survives: ' + ack);
  for (const [key, count] of counts) assert(count === '2', 'Transaction pair remains atomic: ' + key);
  if (boundary === 'create') assert(scalar(await connection.query(`SELECT to_regclass('created_${id}')::text`)) === null, 'Uncommitted DDL is absent');
  if (boundary === 'extension') assert(scalar(await connection.query(`SELECT count(*) FROM extension_${id}`)) === '0', 'Uncommitted relation extension rolls back');
  if (boundary.startsWith('rename') || boundary === 'journal-tail') await probe(14);
  if (boundary === 'checkpoint') assert(scalar(await connection.query("SELECT count(*) FROM payload WHERE value = repeat('b',4000)")) === '200', 'Acknowledged data survives interrupted checkpoint');
  await connection.query('INSERT INTO ledger SELECT id, 3 FROM ledger WHERE false');
  return {rows, ticks, sample: db.sample()};
}
async function close() {
  const metrics = await db.close(); await control.wait(e => e.closed); control.close();
  const budget = storageBudget([metrics], page.probeEvents);
  return {metrics, budget};
}
async function terminate() {
  const before = ticks;
  // This gate deliberately kills the control Worker without its cleanup path.
  (db as any).transport.terminate();
  control.close(); await pending;
  return {method: 'Worker.terminate', pendingSettled: true, pageAlive: ticks >= before};
}
async function startupGate(name: string) {
  const opening = Database.open(options(name, true));
  pending = opening.then(value => {db = value; acknowledged = true;}, error => ({error: String(error)}));
  acknowledged = false;
  await control.wait(e => e.mounted, 60000);
  await control.arm('write', '/PG_VERSION');
  const gate = await control.wait(e => e.gate === 'write', 60000);
  return {gate, acknowledged};
}
page.recovery = {initialize, open, arm, verify, close, terminate, startupGate,
  startupTerminate: () => {ownerWorker.terminate(); control.close(); return {method: 'Worker.terminate', startupPending: true};},
  startupVerify: async () => ({value: scalar(await connection.query('SELECT 23'))}),
  events: () => page.probeEvents,
  observe: async () => ({events: page.probeEvents, storage: await control.inspect(), ticks})};
