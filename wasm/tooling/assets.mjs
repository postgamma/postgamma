#!/usr/bin/env node
/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
import {createHash, randomUUID} from 'node:crypto';
import {lstat, mkdir, readFile, readdir, realpath, rename, unlink, writeFile} from 'node:fs/promises';
import {dirname, isAbsolute, join, relative, resolve, sep} from 'node:path';
import {fileURLToPath, pathToFileURL} from 'node:url';

export const isolationHeaders = Object.freeze({
  'Cross-Origin-Opener-Policy': 'same-origin',
  'Cross-Origin-Embedder-Policy': 'require-corp',
});
const hash = bytes => createHash('sha256').update(bytes).digest('hex');
const inside = (root, path) => {const name = relative(root, path); return !name || (!isAbsolute(name) && name !== '..' && !name.startsWith('..' + sep));};
const safeName = name => typeof name === 'string' && name.length && !name.includes('\\') &&
  !name.startsWith('/') && name.split('/').every(piece => piece && piece !== '.' && piece !== '..');
async function info(path) {try {return await lstat(path);} catch (error) {if (error.code === 'ENOENT') return undefined; throw error;}}
async function noLinks(path) {
  let current = resolve(path);
  for (;;) {
    if ((await info(current))?.isSymbolicLink()) throw new Error('Asset paths must not use symbolic links: ' + current);
    const parent = dirname(current); if (parent === current) return; current = parent;
  }
}
function manifest(bytes) {
  const value = JSON.parse(bytes);
  if (value.schemaVersion !== 1 || !/^[0-9a-f]{16}$/.test(value.buildId) || !value.files || typeof value.files !== 'object')
    throw new Error('Invalid PostGamma asset manifest');
  for (const [name, entry] of Object.entries(value.files))
    if (!safeName(name) || !/^[0-9a-f]{64}$/.test(entry.sha256) || !Number.isSafeInteger(entry.bytes) || entry.bytes < 0)
      throw new Error('Invalid asset entry: ' + name);
  return value;
}
async function licenses(root, prefix = 'licenses') {
  await noLinks(join(root, prefix));
  const files = [];
  for (const entry of await readdir(join(root, prefix), {withFileTypes: true})) {
    const name = prefix + '/' + entry.name;
    if (entry.isDirectory()) files.push(...await licenses(root, name));
    else if (entry.isFile()) files.push(name);
    else throw new Error('Invalid license file: ' + name);
  }
  return files;
}
/** Copy a complete, verified browser asset tree. Unrelated destination files survive. */
export async function copyAssets(destination, {source = fileURLToPath(new URL('../', import.meta.url))} = {}) {
  source = resolve(source); destination = resolve(destination);
  if (inside(source, destination) || inside(destination, source)) throw new Error('Asset source and destination must not overlap');
  await noLinks(source); await noLinks(destination);
  const sourceManifest = await readFile(join(source, 'postgamma.assets.json'));
  const current = manifest(sourceManifest);
  const names = [...Object.keys(current.files), 'LICENSE', 'NOTICE', ...await licenses(source)];
  if (new Set(names).size !== names.length) throw new Error('Duplicate package asset');
  const planned = [];
  for (const name of names) {
    if (!safeName(name)) throw new Error('Invalid asset path');
    const origin = join(source, name), target = join(destination, name);
    await noLinks(origin); await noLinks(target);
    const bytes = await readFile(origin), expected = current.files[name];
    if (expected && (bytes.length !== expected.bytes || hash(bytes) !== expected.sha256))
      throw new Error('Package asset hash mismatch: ' + name);
    planned.push({name, target, bytes, sha256: hash(bytes)});
  }
  // A marker records exactly the files this helper owns, including license notices.
  const markerPath = join(destination, '.postgamma-copy.json'); await noLinks(markerPath);
  let owned = {};
  if (await info(markerPath)) {
    const marker = JSON.parse(await readFile(markerPath, 'utf8'));
    if (marker.schemaVersion !== 1 || !marker.files || typeof marker.files !== 'object') throw new Error('Invalid asset copy marker');
    owned = marker.files;
  }
  planned.push({name: 'postgamma.assets.json', target: join(destination, 'postgamma.assets.json'), bytes: sourceManifest, sha256: hash(sourceManifest)});
  for (const item of planned) {
    await noLinks(item.target);
    const existing = await info(item.target);
    if (!existing) continue;
    if (!existing.isFile()) throw new Error('Asset destination is not a regular file: ' + item.name);
    const previous = hash(await readFile(item.target));
    if (previous !== item.sha256 && previous !== owned[item.name])
      throw new Error('Asset copy would overwrite an unrelated or modified file: ' + item.name);
    item.unchanged = previous === item.sha256;
  }
  const next = {schemaVersion: 1, buildId: current.buildId, files: Object.fromEntries(planned.map(item => [item.name, item.sha256]))};
  planned.push({target: markerPath, bytes: Buffer.from(JSON.stringify(next, null, 2) + '\n')});
  // Validate the full input before writes, and publish the manifest after its assets.
  // Build tooling must finish this call before serving the destination.
  for (const item of planned) {
    if (item.unchanged) continue;
    await mkdir(dirname(item.target), {recursive: true});
    const temporary = item.target + '.tmp-' + randomUUID();
    try {await writeFile(temporary, item.bytes, {flag: 'wx'}); await rename(temporary, item.target);}
    finally {await unlink(temporary).catch(error => {if (error.code !== 'ENOENT') throw error;});}
  }
  return {buildId: current.buildId, directory: destination, files: names.length + 1};
}

if (process.argv[1] && pathToFileURL(await realpath(process.argv[1])).href === import.meta.url) {
  if (process.argv.length !== 3) {console.error('Usage: postgamma-assets <destination-directory>'); process.exitCode = 2;}
  else {
    try {console.log(JSON.stringify(await copyAssets(process.argv[2])));}
    catch (error) {console.error(error.message); process.exitCode = 1;}
  }
}
