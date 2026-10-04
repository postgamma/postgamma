/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
import assert from 'node:assert/strict';
import {copyFile, mkdtemp, rm, writeFile} from 'node:fs/promises';
import {tmpdir} from 'node:os';
import {join, resolve} from 'node:path';
import {pathToFileURL} from 'node:url';

const directory = await mkdtemp(join(tmpdir(), 'pgm-compiled-'));
try {
  await writeFile(join(directory, 'package.json'), '{"type":"module"}');
  await copyFile(resolve(process.argv[2], 'loader/compiled.js'), join(directory, 'compiled.js'));
  await copyFile(resolve(process.argv[2], 'loader/memory.js'), join(directory, 'memory.js'));
  await writeFile(join(directory, 'assets.js'), 'export const verifiedAsset = (...args) => globalThis.fixture.asset(...args);');
  const {compiledKernel} = await import(pathToFileURL(join(directory, 'compiled.js')));
  const {ownedMemory} = await import(pathToFileURL(join(directory, 'memory.js')));
  const memoryManifest = {capabilities: {initialMemoryBytes: 65536, maximumMemoryBytes: 131072}};
  const memory = ownedMemory(memoryManifest), otherMemory = ownedMemory(memoryManifest);
  const clonedMemory = structuredClone(memory);
  new Uint8Array(memory.buffer)[0] = 41;
  assert.equal(new Uint8Array(clonedMemory.buffer)[0], 41, 'Worker clones share the owned memory');
  assert.equal(new Uint8Array(otherMemory.buffer)[0], 0, 'Each database gets fresh memory');
  assert.throws(() => ownedMemory({capabilities: {initialMemoryBytes: 65535, maximumMemoryBytes: 65536}}), /memory policy/);
  // A real Wasm module exports fresh linear memory on each instantiation.
  const bytes = new Uint8Array([0,97,115,109,1,0,0,0,5,3,1,0,1,7,10,1,6,109,101,109,111,114,121,2,0]);
  const base = new URL('https://example.invalid/'), manifest = key => ({
    entries: {wasm: 'kernel.wasm'}, files: {'kernel.wasm': {sha256: key, bytes: bytes.length}},
  });
  let calls = 0, fail = false, invalid = false;
  globalThis.fixture = {asset: async () => {
    calls++; if (fail) throw new Error('Asset integrity mismatch');
    return invalid ? new ArrayBuffer(1) : bytes.buffer;
  }};
  const first = compiledKernel(base, manifest('first'));
  assert.equal(compiledKernel(base, manifest('first')), first, 'Concurrent opens share one compilation');
  const module = await first;
  const one = new WebAssembly.Instance(module), two = new WebAssembly.Instance(module);
  assert.notEqual(one.exports.memory.buffer, two.exports.memory.buffer, 'Only immutable code is shared');
  new Uint8Array(one.exports.memory.buffer)[0] = 42;
  assert.equal(new Uint8Array(two.exports.memory.buffer)[0], 0, 'Instance state remains private');
  assert.equal(await compiledKernel(new URL('https://other.invalid/'), manifest('first')), module);
  assert.equal(calls, 1, 'Identical verified bytes share code across asset directories');
  fail = true;
  await assert.rejects(compiledKernel(base, manifest('bad-integrity')), /integrity/);
  fail = false;
  await compiledKernel(base, manifest('bad-integrity'));
  invalid = true;
  await assert.rejects(compiledKernel(base, manifest('bad-wasm')), WebAssembly.CompileError);
  invalid = false;
  await compiledKernel(base, manifest('bad-wasm'));
  const replacement = await compiledKernel(base, manifest('replacement'));
  assert.notEqual(await compiledKernel(base, manifest('first')), module, 'The cache retains only one module');
  let rejectOld;
  globalThis.fixture.asset = () => new Promise((_, reject) => {rejectOld = reject;});
  const obsolete = compiledKernel(base, manifest('obsolete'));
  globalThis.fixture.asset = async () => bytes.buffer;
  const current = compiledKernel(base, manifest('current'));
  rejectOld(new Error('Obsolete download failed'));
  await assert.rejects(obsolete, /Obsolete/);
  assert.equal(compiledKernel(base, manifest('current')), current, 'Old failures cannot evict a newer module');
  assert.notEqual(await current, replacement);
  console.log('Compiled kernel: bounded cache, concurrent opens, private memory and failure recovery passed');
} finally {delete globalThis.fixture; await rm(directory, {recursive: true, force: true});}
