/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
import assert from 'node:assert/strict';
import {cp, mkdtemp, rm, writeFile} from 'node:fs/promises';
import {tmpdir} from 'node:os';
import {join, resolve} from 'node:path';
import {pathToFileURL} from 'node:url';

const directory = await mkdtemp(join(tmpdir(), 'pgm-transport-'));
try {
  await cp(resolve(process.argv[2]), directory, {recursive: true});
  await writeFile(join(directory, 'package.json'), '{"type":"module"}');
  await writeFile(join(directory, 'loader/assets.js'),
    'import {VERSION} from "../protocol/wire.js"; export const assets = async () => ({protocolVersion: VERSION, entries: {worker: "worker.js"}}); export const verifiedAsset = async () => {};');
  const {Database} = await import(pathToFileURL(join(directory, 'api/database.js')));
  const {Op, VERSION, Writer} = await import(pathToFileURL(join(directory, 'protocol/wire.js')));
  globalThis.location = {href: 'https://example.invalid/'};
  let worker;
  globalThis.Worker = class {
    terminated = 0; commands = []; discards = [];
    constructor() {worker = this;}
    postMessage(data) {
      if (data.discard) {this.discards.push(data.discard); return;}
      const view = new DataView(data), sequence = view.getUint32(4, true), op = view.getUint32(8, true);
      this.commands.push(op);
      if (op === Op.open) {
        const w = new Writer().u32(VERSION).u32(sequence).u32(op).u32(0);
        for (let i = 0; i < 17; i++) w.u32(0xffffffff);
        const reply = w.u32(1).u32(1).u32(1).finish();
        queueMicrotask(() => this.onmessage({data: {sequence, reply}}));
      }
    }
    terminate() {this.terminated++;}
  };
  const db = await Database.open();
  let settled = 0;
  const original = db.telemetry().catch(error => {settled++; return error;});
  worker.onmessageerror();
  const later = db.telemetry().catch(error => {settled++; return error;});
  await new Promise(resolve => setTimeout(resolve, 10));
  assert.equal(worker.discards.length, 1, 'Transport asks the control Worker to release storage');
  assert.equal(worker.terminated, 0, 'Keep the control Worker alive until cleanup is acknowledged');
  assert.equal(settled, 0, 'Pending and new operations do not expose failure before cleanup');
  assert.deepEqual(worker.commands, [Op.open, Op.telemetry], 'Stop admitting native commands during retirement');
  worker.onmessage({data: {fatal: 'Worker cleanup finished', metrics: {storageReleased: true}}});
  const outcomes = await Promise.all([original, later]);
  assert(outcomes.every(error => error instanceof Error));
  assert.equal(outcomes[0].cause.message, 'Worker cleanup finished', 'Retain cleanup diagnostics alongside the initiating error');
  assert.equal(settled, 2);
  assert.equal(worker.terminated, 1);
  assert.equal(db.diagnostics().storageReleased, true);
  worker.onmessage({data: {fatal: 'duplicate'}});
  assert.equal(worker.terminated, 1, 'Retirement is idempotent');
  console.log('Transport retirement: admission stops immediately; rejection waits for storage cleanup');
} finally {await rm(directory, {recursive: true, force: true});}
