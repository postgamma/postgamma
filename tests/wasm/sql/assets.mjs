/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
import assert from 'node:assert/strict';
import {createHash} from 'node:crypto';
import {createServer} from 'node:http';
import {copyFile, mkdtemp, rm, writeFile} from 'node:fs/promises';
import {tmpdir} from 'node:os';
import {join, resolve} from 'node:path';
import {pathToFileURL} from 'node:url';

const directory = await mkdtemp(join(tmpdir(), 'pgm-assets-'));
const originalFetch = globalThis.fetch;
const originalWorker = Object.getOwnPropertyDescriptor(globalThis, 'Worker');
const originalIsolation = Object.getOwnPropertyDescriptor(globalThis, 'crossOriginIsolated');
let server;
try {
  await writeFile(join(directory, 'package.json'), '{"type":"module"}');
  for (const name of ['assets', 'identity'])
    await copyFile(resolve(process.argv[2], 'loader', name + '.js'), join(directory, name + '.js'));
  const {buildId} = await import(pathToFileURL(join(directory, 'identity.js')));
  const {assets, verifiedAsset} = await import(pathToFileURL(join(directory, 'assets.js')));
  Object.defineProperty(globalThis, 'Worker', {value: class {}, configurable: true});
  Object.defineProperty(globalThis, 'crossOriginIsolated', {value: true, configurable: true});
  const bytes = Buffer.from('immutable kernel resource');
  const expected = {bytes: bytes.length, sha256: createHash('sha256').update(bytes).digest('hex')};
  let manifest = {schemaVersion: 1, buildId, protocolVersion: 1,
    entries: {worker: 'resource', module: 'resource', wasm: 'resource', resources: 'resource'},
    files: {resource: expected}};
  let body = bytes, status = 200, hits = 0;
  const requests = [];
  // Exercise the actual Request policy and body consumption against HTTP.
  // The server's no-store header alone allowed Firefox's temporary body copies.
  globalThis.fetch = (input, init) => {
    const request = new Request(input, init);
    requests.push({url: request.url, cache: request.cache});
    return originalFetch(request);
  };
  server = createServer((request, response) => {
    hits++;
    response.setHeader('Cache-Control', 'no-store');
    if (request.url === '/postgamma.assets.json') {
      response.setHeader('Content-Type', 'application/json');
      response.end(JSON.stringify(manifest));
    } else {response.statusCode = status; response.end(body);}
  });
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  const base = new URL('http://127.0.0.1:' + server.address().port + '/');
  const valid = await assets(base);
  assert.deepEqual(Buffer.from(await verifiedAsset(base, valid, 'resource')), bytes);
  const beforeUndeclared = hits;
  await assert.rejects(verifiedAsset(base, valid, 'missing'), /Undeclared/);
  assert.equal(hits, beforeUndeclared, 'Undeclared resources never reach the network');
  body = Buffer.from(bytes);
  body[0] ^= 1;
  await assert.rejects(verifiedAsset(base, valid, 'resource'), /integrity mismatch/);
  body = bytes.subarray(1);
  const wrongSize = {...valid, files: {resource: {...expected,
    sha256: createHash('sha256').update(body).digest('hex')}}};
  await assert.rejects(verifiedAsset(base, wrongSize, 'resource'), /integrity mismatch/,
    'The declared length is checked even when the response matches its SHA-256');
  body = bytes;
  status = 503;
  await assert.rejects(verifiedAsset(base, valid, 'resource'), /503/);
  status = 200;
  assert.deepEqual(Buffer.from(await verifiedAsset(base, valid, 'resource')), bytes,
    'A failed verification does not poison a subsequent valid download');
  manifest = {...manifest, buildId: 'different-build'};
  await assert.rejects(assets(base), /build identity mismatch/);
  manifest = {...manifest, buildId, entries: {...manifest.entries, worker: '../escape'}};
  await assert.rejects(assets(base), /Invalid postgamma asset entry/);
  assert(requests.length >= 8);
  assert(requests.every(request => request.cache === 'no-store'),
    'Both manifests and verified resources bypass HTTP body caches');
  console.log('Assets: fresh manifest, request cache policy, HTTP failures and SHA-256/size validation passed');
} finally {
  globalThis.fetch = originalFetch;
  for (const [name, descriptor] of [['Worker', originalWorker], ['crossOriginIsolated', originalIsolation]]) {
    if (descriptor) Object.defineProperty(globalThis, name, descriptor); else delete globalThis[name];
  }
  if (server) {
    server.closeAllConnections();
    await new Promise(resolve => server.close(resolve));
  }
  await rm(directory, {recursive: true, force: true});
}
