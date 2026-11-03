/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
import assert from 'node:assert/strict';
import {copyFile, mkdir, mkdtemp, rm, writeFile} from 'node:fs/promises';
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
  assert.equal(memory.buffer.byteLength, 65536, 'Owned memory reaches its declared initial size before use');
  assert.equal(memory.grow(1), 1);
  assert.equal(memory.buffer.byteLength, 131072);
  assert.throws(() => memory.grow(1), RangeError, 'Initial growth preserves the declared maximum');
  const importedMemory = new WebAssembly.Module(new Uint8Array([
    0,97,115,109,1,0,0,0,2,16,1,3,101,110,118,6,109,101,109,111,114,121,2,3,2,2,
  ]));
  new WebAssembly.Instance(importedMemory, {env: {memory}});
  assert.throws(() => ownedMemory({capabilities: {initialMemoryBytes: 65535, maximumMemoryBytes: 65536}}), /memory policy/);
  const Memory = WebAssembly.Memory;
  try {
    for (const buffer of [new ArrayBuffer(65536), new SharedArrayBuffer(131072)]) {
      WebAssembly.Memory = class { grow() { return 0; } get buffer() { return buffer; } };
      assert.throws(() => ownedMemory(memoryManifest), /declared shared memory buffer/,
        'The allocating realm validates sharing and initial size before transferring memory to Workers');
    }
    WebAssembly.Memory = class {
      grow() { throw new RangeError('Initial owned memory allocation failed'); }
      get buffer() { throw new Error('Failed growth must not expose a buffer'); }
    };
    assert.throws(() => ownedMemory(memoryManifest), /Initial owned memory allocation failed/);
  } finally { WebAssembly.Memory = Memory; }
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
  // Run the actual loader: a verified compiled module must not fetch another
  // raw binary, while direct loads retain asset integrity and fresh memory.
  const loader = join(directory, 'src/loader');
  await mkdir(loader, {recursive: true});
  await copyFile(resolve(process.argv[2], 'loader/kernel.js'), join(loader, 'kernel.js'));
  await writeFile(join(loader, 'assets.js'), `
    export const assets = (...args) => globalThis.fixture.manifest(...args);
    export const verifiedAsset = (...args) => globalThis.fixture.asset(...args);`);
  await writeFile(join(directory, 'kernel.mjs'),
    'export default async options => globalThis.fixture.create(options);');
  const {loadKernel} = await import(pathToFileURL(join(loader, 'kernel.js')));
  const resources = new Uint8Array([42, 43]).buffer;
  const shared = new WebAssembly.Memory({initial: 1, maximum: 2, shared: true});
  let binaryCalls = 0, supplied, binaryFails = false, failedAsset;
  const verified = [];
  globalThis.fixture = {
    manifest: async () => ({entries: {resources: 'kernel.data', wasm: 'kernel.wasm', module: 'kernel.mjs'}}),
    asset: async (_base, _manifest, name) => {
      verified.push(name);
      if (name === failedAsset) throw new Error('Asset integrity mismatch: ' + name);
      if (name === 'kernel.wasm') {
        binaryCalls++; if (binaryFails) throw new Error('Raw binary was fetched'); return bytes.buffer;
      }
      return name === 'kernel.data' ? resources : new ArrayBuffer(0);
    },
    create: async options => {supplied = options; return {wasmMemory: options.wasmMemory ?? shared};},
  };
  binaryFails = true;
  await loadKernel({print() {}, printErr() {}}, module, shared);
  assert.equal(binaryCalls, 0, 'Compiled opens do not allocate or verify another raw Wasm binary');
  assert.deepEqual(verified, ['kernel.data', 'kernel.mjs'], 'Compiled opens still verify resources and SDK code');
  assert.equal(supplied.wasmBinary, undefined);
  assert.equal(supplied.wasmMemory, shared);
  assert.equal(supplied.getPreloadedPackage('kernel.data', 2), resources);
  assert.throws(() => supplied.getPreloadedPackage('kernel.data', 3), /package size mismatch/);
  let received;
  const exports = supplied.instantiateWasm({}, (instance, receivedModule) => {received = {instance, receivedModule};});
  assert(received.instance instanceof WebAssembly.Instance && received.receivedModule === module);
  assert.equal(exports, received.instance.exports, 'The provided immutable module is instantiated normally');
  for (failedAsset of ['kernel.data', 'kernel.mjs'])
    await assert.rejects(loadKernel({print() {}, printErr() {}}, module, shared), /Asset integrity mismatch/);
  failedAsset = undefined;
  binaryFails = false;
  await loadKernel({print() {}, printErr() {}});
  assert.equal(binaryCalls, 1, 'A direct load verifies its raw binary');
  assert.equal(supplied.wasmBinary, bytes.buffer);
  assert.equal(supplied.instantiateWasm, undefined);
  binaryFails = true;
  await assert.rejects(loadKernel({print() {}, printErr() {}}), /Raw binary was fetched/);
  assert.equal(binaryCalls, 2, 'Direct load integrity failures remain fatal');
  console.log('Kernel loader: compiled opens skip duplicate binary; direct verification, instantiation and resources passed');

  console.log('Compiled kernel: bounded cache, concurrent opens, private memory and failure recovery passed');
} finally {delete globalThis.fixture; await rm(directory, {recursive: true, force: true});}
