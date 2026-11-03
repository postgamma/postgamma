/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
import assert from 'node:assert/strict';
import {cp, mkdtemp, rm, writeFile} from 'node:fs/promises';
import {tmpdir} from 'node:os';
import {join, resolve} from 'node:path';
import {pathToFileURL} from 'node:url';
import {WorkerFixture, waitFor} from './worker-fixture.mjs';
const directory = await mkdtemp(join(tmpdir(), 'pgm-page-'));
try {
  await cp(resolve(process.argv[2]), directory, {recursive: true});
  await writeFile(join(directory, 'package.json'), '{"type":"module"}');
  await writeFile(join(directory, 'loader/assets.js'),
    'import {VERSION} from "../protocol/wire.js"; export const assets = async () => ({protocolVersion: VERSION, capabilities: {initialMemoryBytes: 65536, maximumMemoryBytes: 131072}, entries: {worker: "worker.js", module: "postgamma.mjs"}}); export const verifiedAsset = async () => {};');
  await writeFile(join(directory, 'loader/compiled.js'),
    'export const compiledKernel = async () => new WebAssembly.Module(new Uint8Array([0,97,115,109,1,0,0,0]));');
  const {Database, ClosedError} = await import(pathToFileURL(join(directory, 'api/database.js')));
  const {Op, VERSION, Writer} = await import(pathToFileURL(join(directory, 'protocol/wire.js')));
  globalThis.location = {href: 'https://example.invalid/'};
  globalThis.window = new EventTarget();
  const hidden = persisted => {
    const event = new Event('pagehide'); Object.defineProperty(event, 'persisted', {value: persisted});
    window.dispatchEvent(event);
  };
  let worker, pendingPrepare = false;
  globalThis.Worker = class extends WorkerFixture {
    prepared = false;
    constructor() {super(); worker = this;}
    receive(data) {
      if (data.postgammaPrepare) {this.prepared = true; if (!pendingPrepare) this.emit({postgammaPrepared: true}); return;}
      if (data.postgammaPthreadBroker) return;
      const v = new DataView(data), sequence = v.getUint32(4, true), op = v.getUint32(8, true);
      if (op === Op.open) {
        const w = new Writer().u32(VERSION).u32(sequence).u32(op).u32(0);
        for (let i = 0; i < 17; i++) w.u32(0xffffffff);
        this.emit({sequence, reply: w.u32(1).u32(1).u32(1).finish()});
      }
    }
  };
  for (const options of [{storage: {type: 'opfs', name: 'test'}}, {bfcache: 'discard'}]) {
    const db = await Database.open(options), owner = worker;
    hidden(false); assert.equal(owner.terminated, 0);
    const pending = assert.rejects(db.telemetry(), error => error instanceof ClosedError && /bfcache/.test(error.message));
    hidden(true); assert.equal(owner.terminated, 1, 'Termination is synchronous with pagehide');
    await pending;
    await assert.rejects(db.connect(), /discarded on entering bfcache/);
    hidden(true); assert.equal(owner.terminated, 1);
  }
  for (const options of [{}, {storage: {type: 'opfs', name: 'test'}, bfcache: 'preserve'}]) {
    const db = await Database.open(options); hidden(true); assert.equal(worker.terminated, 0);
    db.transport.terminate();
  }
  pendingPrepare = true;
  const opening = assert.rejects(Database.open({storage: {type: 'opfs', name: 'waiting'}}), /bfcache/);
  await waitFor(() => worker.prepared && worker.terminated === 0);
  hidden(true); await opening; assert(worker.terminated > 0, 'Pending lock owner is also terminated');
  await assert.rejects(Database.open({bfcache: 'invalid'}), TypeError);
  console.log('Page lifecycle: persistent default, memory preservation, opt-in/out, stale handles and pending startup passed');
} finally {await rm(directory, {recursive: true, force: true});}
