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
  const memory = new WebAssembly.Memory({initial: initial / 65536, maximum: maximum / 65536, shared: true});
  // Materialize the buffer in this realm before cloning the memory to Workers.
  // WebKit registers Memory's initial GC allocation before adopting its backing
  // store. Creating the buffer wrapper accounts for the actual allocation.
  const buffer: unknown = memory.buffer;
  if (!(buffer instanceof SharedArrayBuffer) || buffer.byteLength !== initial)
    throw new Error('Kernel requires the declared shared memory buffer');
  return memory;
}
