/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
import {verifiedAsset} from './assets.js';
import type {AssetManifest} from './assets.js';

// Keep one immutable code module in the API owner's lifetime. Each control
// Worker and pthread still instantiates its own runtime and shared memory.
// Recompiling the large module on every reopen retains browser compiler memory.
let cached: {key: string; work: Promise<WebAssembly.Module>} | undefined;
export function compiledKernel(base: URL, manifest: AssetManifest): Promise<WebAssembly.Module> {
  const name = manifest.entries.wasm, expected = manifest.files[name];
  const key = expected.sha256 + ':' + expected.bytes;
  if (cached?.key === key) return cached.work;
  const entry = {key, work: verifiedAsset(base, manifest, name).then(bytes => WebAssembly.compile(bytes))};
  entry.work = entry.work.catch(error => {
    if (cached === entry) cached = undefined;
    throw error;
  });
  cached = entry;
  return entry.work;
}
