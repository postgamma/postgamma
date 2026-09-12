/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
import type {KernelModule} from '../bindings/kernel.js';
import {assets, verifiedAsset} from './assets.js';

export async function loadKernel(output: {
  print(text: string): void; printErr(text: string): void; onAbort?(reason: unknown): void;
}): Promise<KernelModule> {
  const assetRoot = new URL('../../', import.meta.url);
  const manifest = await assets(assetRoot);
  const [resources, wasmBinary] = await Promise.all([
    verifiedAsset(assetRoot, manifest, manifest.entries.resources),
    verifiedAsset(assetRoot, manifest, manifest.entries.wasm),
    verifiedAsset(assetRoot, manifest, manifest.entries.module),
  ]);
  const {default: createKernel} = await import(new URL(manifest.entries.module, assetRoot).href);
  const module: KernelModule = await createKernel({
    ...output, wasmBinary, noExitRuntime: true,
    locateFile: (path: string) => new URL(path, assetRoot).href,
    getPreloadedPackage: (_name: string, size: number) => {
      if (resources.byteLength !== size) throw new Error('Kernel resource package size mismatch');
      return resources;
    },
  });
  if (!(module.wasmMemory.buffer instanceof SharedArrayBuffer))
    throw new Error('Kernel requires shared Wasm memory');
  return module;
}
