/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
import type {AssetManifest} from './assets.js';

export function ownedMemory(manifest: AssetManifest): WebAssembly.Memory {
  const {initialMemoryBytes: initial, maximumMemoryBytes: maximum} = manifest.capabilities;
  if (![initial, maximum].every(value => Number.isSafeInteger(value) && value > 0 && value % 65536 === 0)
      || initial > maximum || maximum > 4294967296)
    throw new RangeError('Invalid kernel memory policy');
  // Allocate in the API owner's realm, which outlives its control Workers.
  // Some WebKit builds retain shared memories allocated by retired Workers.
  // Every open gets fresh memory; only immutable compiled code is cached.
  // WebKit adopts constructor backing memory after reporting its GC cost.
  // Growing an empty memory reports the real allocation to the owning VM;
  // otherwise retired instances can wait behind an understated GC budget.
  const memory = new WebAssembly.Memory({initial: 0, maximum: maximum / 65536, shared: true});
  memory.grow(initial / 65536);
  // Materialize and validate the shared buffer before cloning it to Workers.
  const buffer: unknown = memory.buffer;
  if (!(buffer instanceof SharedArrayBuffer) || buffer.byteLength !== initial)
    throw new Error('Kernel requires the declared shared memory buffer');
  return memory;
}
