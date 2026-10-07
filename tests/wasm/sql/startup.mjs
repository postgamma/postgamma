/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
import assert from 'node:assert/strict';
import {copyFile, mkdir, mkdtemp, rm, writeFile} from 'node:fs/promises';
import {tmpdir} from 'node:os';
import {dirname, join, resolve} from 'node:path';
import {pathToFileURL} from 'node:url';

// Execute the actual control Worker with a controlled native bridge and lock.
// Failed native OPEN and failed STOP are independent replies in this fixture.
const source = resolve(process.argv[2]), directory = await mkdtemp(join(tmpdir(), 'pgm-startup-'));
try {
  await writeFile(join(directory, 'package.json'), '{"type":"module"}');
  for (const name of ['worker/sql.js', 'protocol/wire.js', 'protocol/limits.js', 'runtime/pthreads.js', 'runtime/messages.js']) {
    await mkdir(dirname(join(directory, name)), {recursive: true});
    await copyFile(join(source, name), join(directory, name));
  }
  for (const [name, content] of Object.entries({
    'loader/kernel.js': 'export const loadKernel = () => globalThis.fixture.loadKernel();',
    'bindings/sql.js': 'export const exchange = (...args) => globalThis.fixture.exchange(...args); export const tick = () => new Promise(resolve => setTimeout(resolve, 1));',
    'storage/ownership.js': 'export const acquire = () => globalThis.fixture.acquire();',
  })) {
    await mkdir(dirname(join(directory, name)), {recursive: true});
    await writeFile(join(directory, name), content);
  }
  const {Writer, Reader, Op, VERSION, response, DatabaseError} = await import(pathToFileURL(join(directory, 'protocol/wire.js')));
  const diagnostics = [null, null, 'Database directory does not exist',
    'Original startup detail; OPFS startup rollback failed: I/O error'];
  function reply(op, status, fields = []) {
    const w = new Writer().u32(VERSION).u32(1).u32(op).u32(status);
    for (let i = 0; i < 17; i++) fields[i] == null ? w.u32(0xffffffff) : w.text(fields[i]);
    return w.finish();
  }
  for (const stopFailed of [false, true]) {
    globalThis.postgammaNativeRetired = false;
    let releaseLock, reachedRelease, deliver, closed = 0, terminated = 0;
    let retired = 0, joinAttempts = 0, nativeJoined = false, sdkReturned = false;
    let returnWorkers;
    const lock = new Promise(resolve => {releaseLock = resolve;});
    const releasing = new Promise(resolve => {reachedRelease = resolve;});
    const delivered = new Promise(resolve => {deliver = resolve;});
    const messages = [], calls = [];
    const module = {
      wasmMemory: {buffer: new ArrayBuffer(1024)},
      PThread: {pthreads: {1: {}}, unusedWorkers: [], terminateAllThreads() {
        terminated++;
        for (const worker of this.unusedWorkers) worker.terminate();
        this.unusedWorkers = []; this.pthreads = {};
      }},
      _postgamma_wasm_start: () => 0, _postgamma_wasm_join: () => {
        if (++joinAttempts < 3) return 10;
        nativeJoined = true; setTimeout(returnWorkers, 0); return 0;
      },
      _postgamma_wasm_wake_count: () => 0, _postgamma_wasm_storage_stack_bytes: () => 2097152,
    };
    globalThis.fixture = {
      loadKernel: async () => module,
      acquire: async () => async () => {
        if (!stopFailed) assert.equal(retired, 2, 'Idle pthread termination precedes ownership release');
        reachedRelease(); await lock;
      },
      exchange: async (_, request) => {
        const r = new Reader(request); r.u32(); r.u32(); const op = r.u32(); calls.push(op);
        if (op === Op.open) return reply(op, 10, diagnostics);
        assert.equal(op, Op.stop);
        if (stopFailed) return reply(op, 10, [null, null, 'OPFS storage close failed']);
        returnWorkers = () => {
          assert(nativeJoined, 'Native join completes before SDK pool return');
          module.PThread.pthreads = {}; sdkReturned = true;
          module.PThread.unusedWorkers = [1, 2].map(() => ({
            postMessage() {assert.fail('An idle pthread needs no further native or JS reply');},
            terminate() {
              assert(nativeJoined && sdkReturned, 'Native owners and SDK mappings must drain before termination');
              assert.equal(globalThis.postgammaNativeRetired, true);
              retired++;
            },
          }));
        };
        return reply(op, 0);
      },
    };
    globalThis.self = {addEventListener() {}, close() {closed++;}};
    globalThis.postMessage = message => {messages.push(message); deliver(message);};
    await import(pathToFileURL(join(directory, 'worker/sql.js')) + '?stopFailed=' + stopFailed);
    const request = new Writer().u32(VERSION).u32(1).u32(Op.open)
      .u32(0).u32(4).text('/database').u32(1).text('missing').text('').u32(0).finish();
    let timer;
    try {
      const broker = new MessageChannel();
      self.onmessage({data: {postgammaPthreadBroker: true}, ports: [broker.port2]});
      broker.port1.close(); broker.port2.close();
      const timeout = new Promise((_, reject) => {timer = setTimeout(() => reject(new Error('Startup cleanup stalled')), 2000);});
      self.onmessage({data: request});
      await Promise.race([releasing, timeout]);
      assert.equal(messages.length, 0, 'Startup failure waits for lock release');
      releaseLock();
      const result = await Promise.race([delivered, timeout]);
      assert.deepEqual(calls, [Op.open, Op.stop]);
      assert.equal(terminated, 1);
      assert.equal(closed, 0, 'The parent transport owns control Worker termination');
      if (stopFailed) {
        for (const text of [diagnostics[2], diagnostics[3], 'OPFS storage close failed'])
          assert(result.fatal.includes(text), 'Forced teardown retains diagnostic: ' + text);
      } else {
        assert.equal(result.sequence, 1);
        assert.throws(() => response(result.reply, 1, Op.open), error =>
          error instanceof DatabaseError && error.diagnostics[3] === diagnostics[3]);
      }
    } finally {clearTimeout(timer); releaseLock();}
  }
  console.log('Startup cleanup: original OPEN diagnostics, failed STOP and lock release passed');
} finally {await rm(directory, {recursive: true, force: true});}
