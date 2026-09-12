/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
import {buildId} from './identity.js';
export interface AssetManifest {
  schemaVersion: number; buildId: string; protocolVersion: number;
  entries: {worker: string; module: string; wasm: string; resources: string};
  files: Record<string, {sha256: string; bytes: number}>;
}
export async function assets(base: URL): Promise<AssetManifest> {
  if (!globalThis.crossOriginIsolated || typeof SharedArrayBuffer !== 'function' ||
      typeof Worker !== 'function' || typeof WebAssembly !== 'object' ||
      !globalThis.crypto?.subtle || !globalThis.crypto?.getRandomValues)
    throw new Error('postgamma requires cross-origin isolation, shared Wasm memory, Workers and Web Crypto');
  const response = await fetch(new URL('postgamma.assets.json', base));
  if (!response.ok) throw new Error(`Asset manifest download failed: ${response.status}`);
  const manifest = await response.json() as AssetManifest;
  if (manifest.schemaVersion !== 1 || manifest.buildId !== buildId)
    throw new Error('postgamma asset build identity mismatch');
  for (const name of Object.values(manifest.entries))
    if (!/^[a-zA-Z0-9_./-]+$/.test(name) || name.startsWith('/') || name.split('/').includes('..') || !manifest.files[name])
      throw new Error('Invalid postgamma asset entry');
  return manifest;
}
export async function verifiedAsset(base: URL, manifest: AssetManifest, name: string): Promise<ArrayBuffer> {
  const expected = manifest.files[name];
  if (!expected) throw new Error('Undeclared postgamma asset: ' + name);
  const response = await fetch(new URL(name, base));
  if (!response.ok) throw new Error(`Asset download failed (${name}): ${response.status}`);
  const bytes = await response.arrayBuffer();
  const digest = new Uint8Array(await crypto.subtle.digest('SHA-256', bytes));
  const hash = Array.from(digest, byte => byte.toString(16).padStart(2, '0')).join('');
  if (bytes.byteLength !== expected.bytes || hash !== expected.sha256)
    throw new Error('postgamma asset integrity mismatch: ' + name);
  return bytes;
}
