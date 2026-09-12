/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
import type {KernelModule} from './kernel.js';
export interface SqlModule extends KernelModule {
  _postgamma_wasm_start(): number;
  _postgamma_wasm_alloc(size: number): number;
  _postgamma_wasm_free(pointer: number): void;
  _postgamma_wasm_submit(pointer: number, size: number): number;
  _postgamma_wasm_poll(): number;
  _postgamma_wasm_reply(): number;
  _postgamma_wasm_reply_size(): number;
  _postgamma_wasm_ack(): void;
  _postgamma_wasm_join(): number;
}
export const tick = () => new Promise<void>(resolve => setTimeout(resolve, 1));
export async function exchange(module: SqlModule, message: ArrayBuffer, signal?: AbortSignal): Promise<ArrayBuffer> {
  const pointer = module._postgamma_wasm_alloc(message.byteLength);
  if (!pointer) throw new Error('Bridge allocation failed');
  new Uint8Array(module.wasmMemory.buffer).set(new Uint8Array(message), pointer);
  const status = module._postgamma_wasm_submit(pointer, message.byteLength);
  if (status) {module._postgamma_wasm_free(pointer); throw new Error(`Bridge submission failed: ${status}`);}
  while (module._postgamma_wasm_poll() === 1) {
    await tick();
    if (signal?.aborted) throw new Error('Kernel instance failed');
  }
  if (module._postgamma_wasm_poll() !== 2) throw new Error('Bridge driver failed');
  try {
    // Acquire the current memory buffer, even when another pthread grew it. The returned buffer owns its
    // bytes and survives result release, Worker transfer and Wasm memory growth.
    const start = module._postgamma_wasm_reply(), size = module._postgamma_wasm_reply_size();
    const copy = new Uint8Array(size); copy.set(new Uint8Array(module.wasmMemory.buffer, start, size));
    return copy.buffer;
  } finally {module._postgamma_wasm_ack();}
}
