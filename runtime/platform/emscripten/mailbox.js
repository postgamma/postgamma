/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
// Use the pinned SDK's postMessage mailbox path. Native waitAsync notification
// can stop making progress during WebKit kernel startup. Leaving waiting_async
// clear selects the SDK's event delivery, including cancellation and Worker reuse.
// Owned I/O uses a separate direct channel when the control relay is occupied.
// Doctor and kernel linking verify the reviewed SDK files recorded in
// manifests/profiles/wasm-sdk-contract.json before accepting this override.
addToLibrary({
  _emscripten_thread_mailbox_await: (pthread) => {},
  // Read only while the control Worker still owns this live PThread entry.
  // The pinned wasm32 pthread layout ends in the atomic wait_addr member;
  // the SDK header and generated layout are checked by the adapter contract.
  postgamma_thread_diagnostic: (thread, field) => {
    // Another pthread may have grown shared memory since this Worker last ran.
    const heap = new Uint32Array(wasmMemory.buffer);
    if (field === 0)
      return Atomics.load(heap, (thread + {{{ C_STRUCTS.pthread.__size__ - 4 }}}) >> 2);
    if (field === 1)
      return Atomics.load(heap, (thread + {{{ C_STRUCTS.pthread.waiting_async }}}) >> 2);
    return heap[(thread + {{{ C_STRUCTS.pthread.stack_size }}}) >> 2];
  },
});
