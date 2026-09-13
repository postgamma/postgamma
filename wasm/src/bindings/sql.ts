/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
import type {KernelModule} from './kernel.js';
export interface SqlModule extends KernelModule {
  postgammaBridgeEvent?: (kind: number, id: number, generation: number) => void;
  postgammaBridgeReply?: () => void;
  _postgamma_wasm_start(): number;
  _postgamma_wasm_alloc(size: number): number;
  _postgamma_wasm_free(pointer: number): void;
  _postgamma_wasm_submit(pointer: number, size: number): number;
  _postgamma_wasm_poll(): number;
  _postgamma_wasm_reply(): number;
  _postgamma_wasm_reply_size(): number;
  _postgamma_wasm_ack(): void;
  _postgamma_wasm_join(): number;
  _postgamma_wasm_wake_count(): number;
  _postgamma_wasm_storage_stack_bytes(): number;
}
export const tick = () => new Promise<void>(resolve => setTimeout(resolve, 1));
export async function exchange(module: SqlModule, message: ArrayBuffer, signal?: AbortSignal): Promise<ArrayBuffer> {
  if (signal?.aborted) throw new Error('Kernel instance failed');
  const pointer = module._postgamma_wasm_alloc(message.byteLength);
  if (!pointer) throw new Error('Bridge allocation failed');
  new Uint8Array(module.wasmMemory.buffer).set(new Uint8Array(message), pointer);
  let abort!: () => void;
  const ready = new Promise<void>((resolve, reject) => {
    module.postgammaBridgeReply = resolve;
    abort = () => reject(new Error('Kernel instance failed'));
    signal?.addEventListener('abort', abort, {once: true});
  });
  let submitted = false;
  try {
    const status = module._postgamma_wasm_submit(pointer, message.byteLength);
    if (status) throw new Error(`Bridge submission failed: ${status}`);
    submitted = true; // The driver now owns the input until acknowledgement.
    await ready;
    if (module._postgamma_wasm_poll() !== 2) throw new Error('Bridge driver failed');
    // Acquire the current memory buffer, even when another pthread grew it. The returned buffer owns its
    // bytes and survives result release, Worker transfer and Wasm memory growth.
    const start = module._postgamma_wasm_reply(), size = module._postgamma_wasm_reply_size();
    const copy = new Uint8Array(size); copy.set(new Uint8Array(module.wasmMemory.buffer, start, size));
    module._postgamma_wasm_ack();
    return copy.buffer;
  } finally {
    if (!submitted) module._postgamma_wasm_free(pointer);
    module.postgammaBridgeReply = undefined;
    signal?.removeEventListener('abort', abort);
  }
}
