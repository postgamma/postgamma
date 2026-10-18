/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
import * as sdk from '../src/api/database.js';
import {Control} from './control.js';
import {scalar} from '../tests/helpers.js';

const page = window as any;
page.probeEvents = [];
const Memory = WebAssembly.Memory, NativeWorker = Worker;
let memories = 0, workers = 0, pthreads = 0;
WebAssembly.Memory = class extends Memory {
  constructor(options: WebAssembly.MemoryDescriptor) {super(options); memories++;}
};
window.Worker = class extends NativeWorker {
  constructor(url: string | URL, options?: WorkerOptions) {
    super(url, options); workers++;
    if (options?.name?.startsWith('em-pthread-')) pthreads++;
  }
};
let db: sdk.Database | undefined, connection: sdk.Connection | undefined, control: Control | undefined;
let controller: AbortController, result: unknown = null, opening: Promise<void>;
let counts = {memories: 0, workers: 0, pthreads: 0};
let pending: Promise<unknown> | undefined;
const snapshot = () => ({memories: memories - counts.memories, workers: workers - counts.workers,
  pthreads: pthreads - counts.pthreads});
function errorInfo(error: any) {
  const Locked = (sdk as any).DatabaseLockedError;
  return {name: error?.name, code: error?.code, databaseName: error?.databaseName,
    reason: error?.reason, message: String(error), locked: typeof Locked === 'function' && error instanceof Locked};
}
function lifecycle(event: PageTransitionEvent) {
  const events = JSON.parse(sessionStorage.getItem('multitab-events') ?? '[]');
  events.push({type: event.type, persisted: event.persisted});
  sessionStorage.setItem('multitab-events', JSON.stringify(events));
}
addEventListener('pagehide', lifecycle); addEventListener('pageshow', lifecycle);
page.multitab = {
  begin(options: sdk.OpenOptions & {name?: string; fault?: boolean; preabort?: boolean}) {
    if (db || result === 'opening') throw new Error('Fixture already owns an instance');
    counts = {memories, workers, pthreads}; result = 'opening';
    controller = new AbortController();
    if (options.preabort) controller.abort();
    const {name, fault, preabort, ...rest} = options;
    const channel = crypto.randomUUID();
    if (fault) control = new Control(channel);
    opening = sdk.Database.open({...rest, signal: controller.signal,
      ...(name ? {storage: {type: 'opfs', name}} : {}),
      ...(fault ? {storageTestChannel: channel} : {})} as sdk.OpenOptions).then(value => {
      db = value; result = {status: 'opened', counts: snapshot(), linearMemory: db.sample()?.memory};
    }, error => {result = {status: 'rejected', error: errorInfo(error), counts: snapshot()};});
  },
  state: () => ({result, counts: snapshot()}),
  abort: () => controller.abort(),
  async sql(sql: string) {
    connection ??= await db!.connect('multitab');
    return (await connection.exec(sql)).at(-1)?.rows;
  },
  async init() {
    connection ??= await db!.connect('multitab');
    await connection.query('CREATE TABLE ledger(id int PRIMARY KEY, value text)');
    await connection.query("INSERT INTO ledger VALUES(1,'ack'),(2,'ack')");
  },
  async armCommit(boundary: string) {
    await control!.wait(event => event.mounted, 60000);
    await connection!.query('BEGIN');
    await connection!.query("INSERT INTO ledger VALUES(3,'pending'),(4,'pending')");
    await control!.arm(boundary, '/pg_wal/', 'pause');
    pending = connection!.query('COMMIT').catch(error => errorInfo(error));
    return await control!.wait(event => event.gate === boundary, 15000);
  },
  async close() {
    await opening;
    try {return await db?.close();}
    finally {
      db = undefined; connection = undefined; control?.close(); control = undefined;
      result = null; await pending; pending = undefined;
    }
  },
  async values() {
    connection ??= await db!.connect('multitab');
    return {ack: scalar(await connection.query("SELECT count(*) FROM ledger WHERE value='ack'")),
      pending: scalar(await connection.query("SELECT count(*) FROM ledger WHERE value='pending'"))};
  },
  lifecycle: () => JSON.parse(sessionStorage.getItem('multitab-events') ?? '[]'),
  list: () => sdk.Database.list(),
  delete: (name: string, options: sdk.DeleteOptions = {}) => sdk.Database.delete(name, options),
  persistence: (request = false) => sdk.Database.storagePersistence({request}),
  pagehide: (persisted: boolean) => window.dispatchEvent(new PageTransitionEvent('pagehide', {persisted})),
};
