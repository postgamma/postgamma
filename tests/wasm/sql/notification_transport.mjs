/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
import assert from 'node:assert/strict';
import {cp, mkdtemp, rm, writeFile} from 'node:fs/promises';
import {tmpdir} from 'node:os';
import {join, resolve} from 'node:path';
import {pathToFileURL} from 'node:url';
import {WorkerFixture, waitFor} from './worker-fixture.mjs';
const directory = await mkdtemp(join(tmpdir(), 'pgm-notifications-'));
try {
  await cp(resolve(process.argv[2]), directory, {recursive: true});
  await writeFile(join(directory, 'package.json'), '{"type":"module"}');
  await writeFile(join(directory, 'loader/assets.js'), 'import {VERSION} from "../protocol/wire.js"; export const assets = async () => ({protocolVersion: VERSION, capabilities: {initialMemoryBytes: 65536, maximumMemoryBytes: 131072}, entries: {worker: "worker.js", module: "postgamma.mjs"}}); export const verifiedAsset = async () => {};');
  await writeFile(join(directory, 'loader/compiled.js'),
    'export const compiledKernel = async () => new WebAssembly.Module(new Uint8Array([0,97,115,109,1,0,0,0]));');
  const {Database, Connection} = await import(pathToFileURL(join(directory, 'api/database.js')));
  const {Op, VERSION, Writer} = await import(pathToFileURL(join(directory, 'protocol/wire.js')));
  const {limits} = await import(pathToFileURL(join(directory, 'protocol/limits.js')));
  globalThis.location = {href: 'https://example.invalid/'};
  let worker;
  globalThis.Worker = class extends WorkerFixture {
    commands = [];
    constructor() {super(); worker = this;}
    receive(data) {
      if (data.postgammaPrepare) {this.emit({postgammaPrepared: true}); return;}
      if (data.postgammaPthreadBroker) return;
      assert(data instanceof ArrayBuffer);
      const view = new DataView(data), sequence = view.getUint32(4, true), op = view.getUint32(8, true);
      const command = {sequence, op}; this.commands.push(command);
      if (op === Op.open) queueMicrotask(() => this.reply(command, w => w.u32(1).u32(1).u32(1)));
    }
    reply(command, payload = () => {}) {
      const w = new Writer().u32(VERSION).u32(command.sequence).u32(command.op).u32(0);
      for (let i = 0; i < 17; i++) w.u32(0xffffffff);
      payload(w); this.emit({sequence: command.sequence, reply: w.finish()});
    }
  };
  const db = await Database.open(), transport = db.transport;
  const connection = new Connection(transport, {kind: 2, id: 2, generation: 1}, () => {});
  const received = []; const unsubscribe = connection.onNotification(n => received.push(n));
  const requests = Array.from({length: limits.maxPendingCommands}, () => transport.call(Op.telemetry));
  const completions = Promise.allSettled(requests);
  await waitFor(() => worker.commands.filter(c => c.op === Op.telemetry).length === limits.maxPendingCommands);
  worker.onmessage({data: {notificationsReady: true}});
  assert(!worker.commands.some(c => c.op === Op.events), 'Full command queue defers notification drain');
  worker.reply(worker.commands.find(c => c.op === Op.telemetry));
  await waitFor(() => worker.commands.some(c => c.op === Op.events));
  const drain = worker.commands.find(c => c.op === Op.events);
  assert(drain, 'A command reply resumes notification drain without a polling timer');
  worker.reply(drain, w => w.u32(1).u32(1).u32(2).u32(2).u32(1).u32(101).text('test').text('value'));
  await waitFor(() => received.length === 1);
  assert.deepEqual(received, [{pid: 101, channel: 'test', payload: 'value'}]);
  unsubscribe(); transport.closeEvents();
  const count = worker.commands.length;
  worker.onmessage({data: {notificationsReady: true}});
  assert.equal(worker.commands.length, count, 'Readiness after close cannot submit commands to the stopped driver');
  transport.terminate(); await completions;
  assert(worker.terminated);
  console.log('Notification transport: deferred queue drain and close retirement passed');
} finally {await rm(directory, {recursive: true, force: true});}
