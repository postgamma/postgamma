/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
// Use the pinned SDK's postMessage mailbox path. Native waitAsync notification
// can stop making progress during WebKit kernel startup. Leaving waiting_async
// clear selects the SDK's event delivery, including cancellation and Worker reuse.
// Owned I/O uses a separate direct channel when the control relay is occupied.
addToLibrary({
  _emscripten_thread_mailbox_await: (pthread) => {},
});
