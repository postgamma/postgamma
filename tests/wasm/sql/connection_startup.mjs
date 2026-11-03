/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
import assert from 'node:assert/strict';
import {cp, mkdtemp, rm, writeFile} from 'node:fs/promises';
import {tmpdir} from 'node:os';
import {join, resolve} from 'node:path';
import {pathToFileURL} from 'node:url';
import {WorkerFixture, waitFor} from './worker-fixture.mjs';

const directory = await mkdtemp(join(tmpdir(), 'pgm-connect-deadline-'));
const setTimer = globalThis.setTimeout, clearTimer = globalThis.clearTimeout;
try {
  await cp(resolve(process.argv[2]), directory, {recursive: true});
  await writeFile(join(directory, 'package.json'), '{"type":"module"}');
  await writeFile(join(directory, 'loader/assets.js'),
    'import {VERSION} from "../protocol/wire.js"; export const assets = async () => ({protocolVersion: VERSION, capabilities: {initialMemoryBytes: 65536, maximumMemoryBytes: 131072}, entries: {worker: "worker.js", module: "postgamma.mjs"}}); export const verifiedAsset = async () => {};');
  await writeFile(join(directory, 'loader/compiled.js'),
    'export const compiledKernel = async () => new WebAssembly.Module(new Uint8Array([0,97,115,109,1,0,0,0]));');
  const {Database} = await import(pathToFileURL(join(directory, 'api/database.js')));
  const {Op, VERSION, Writer} = await import(pathToFileURL(join(directory, 'protocol/wire.js')));
  globalThis.location = {href: 'https://example.invalid/'};
  for (const phase of ['begin', 'poll', 'readiness', 'complete']) {
    const timers = new Set();
    globalThis.setTimeout = (callback, ms, ...args) => {
      if (ms !== 90000) return setTimer(callback, ms, ...args);
      const timer = {callback}; timers.add(timer); return timer;
    };
    globalThis.clearTimeout = timer => {
      if (timers.has(timer)) timers.delete(timer); else clearTimer(timer);
    };
    let worker, id = 1;
    globalThis.Worker = class extends WorkerFixture {
      commands = []; discards = [];
      constructor() {super(); worker = this;}
      receive(data) {
        if (data.postgammaPrepare) {this.emit({postgammaPrepared: true}); return;}
        if (data.postgammaPthreadBroker) return;
        if (data.discard) {this.discards.push(data.discard); return;}
        const view = new DataView(data), sequence = view.getUint32(4, true), op = view.getUint32(8, true);
        this.commands.push(op);
        const w = new Writer().u32(VERSION).u32(sequence).u32(op).u32(0);
        for (let i = 0; i < 17; i++) w.u32(0xffffffff);
        if (op === Op.open) w.u32(1).u32(1).u32(1);
        else if (op === Op.connect) {
          if (phase === 'begin') return;
          w.u32(5).u32(++id).u32(1);
        } else if (op === Op.connectPoll) {
          if (phase === 'poll') return;
          if (phase === 'readiness') w.u32(1);
          else w.u32(2).u32(2).u32(view.getUint32(12, true)).u32(1);
        } else assert.fail('Unexpected command: ' + op);
        const reply = w.finish();
        this.emit({sequence, reply});
      }
    };
    const db = await Database.open();
    let settled = 0;
    const connections = [db.connect('one'), db.connect('two')].map(work => work.then(
      connection => {settled++; return connection;}, error => {settled++; return error;}));
    if (phase !== 'complete') await waitFor(() => phase === 'begin' ?
      worker.commands.filter(op => op === Op.connect).length === 2 :
      worker.commands.filter(op => op === Op.connectPoll).length === 2);
    if (phase === 'complete') {
      const opened = await Promise.all(connections);
      assert(opened.every(connection => connection.handle?.kind === 2), opened.map(String).join('; '));
      assert.equal(timers.size, 0, 'Successful startup cancels its watchdog');
      db.transport.terminate();
    } else {
      assert.equal(settled, 0);
      assert.equal(timers.size, 2);
      for (const timer of timers) timer.callback();
      const commandCount = worker.commands.length;
      const later = db.telemetry().catch(error => error);
      await waitFor(() => worker.discards.length === 1);
      assert.equal(worker.discards.length, 1, 'Concurrent expired startups share one discard');
      assert.equal(worker.commands.length, commandCount, 'No native admission after the deadline');
      assert.equal(worker.terminated, 0, 'Storage cleanup owns the live control Worker');
      assert.equal(settled, 0, 'Caller rejection waits for physical storage cleanup');
      worker.onmessage({data: {fatal: 'cleanup complete', metrics: {storageReleased: true}}});
      const outcomes = await Promise.all([...connections, later]);
      assert(outcomes.every(error => error instanceof Error && /90s deadline/.test(error.message)));
      assert.equal(worker.terminated, 1);
      assert.equal(db.diagnostics().storageReleased, true);
      assert.equal(db.starting.size, 0, 'Failed startups release their API reservations');
      assert.equal(timers.size, 0, 'Discard cancels every startup watchdog');
    }
  }
  console.log('Connection startup: bounded begin, poll and readiness waits; shared cleanup before rejection; successful timer retirement');
} finally {
  globalThis.setTimeout = setTimer; globalThis.clearTimeout = clearTimer;
  await rm(directory, {recursive: true, force: true});
}
