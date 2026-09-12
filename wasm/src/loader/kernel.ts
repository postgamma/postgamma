/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
import type {KernelModule} from '../bindings/kernel.js';

export async function loadKernel(output: {
  print(text: string): void; printErr(text: string): void;
}): Promise<KernelModule> {
  if (!globalThis.crossOriginIsolated || !globalThis.crypto?.getRandomValues)
    throw new Error('Kernel Worker requires cross-origin isolation and Web Crypto');
  const assetRoot = new URL('../../', import.meta.url);
  const response = await fetch(new URL('postgamma.data', assetRoot));
  if (!response.ok) throw new Error(`Kernel resource download failed: ${response.status}`);
  const resources = await response.arrayBuffer();
  const {default: createKernel} = await import('../../postgamma.mjs');
  const module: KernelModule = await createKernel({
    ...output,
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
