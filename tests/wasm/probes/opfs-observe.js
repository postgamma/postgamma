/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
/* Probe-only observation of the unmodified SDK's browser API calls. Each
 * pthread installs these wrappers locally; access handles remain on the SDK
 * I/O owner. Telemetry writes shared C records without calling a caller. */
if (typeof FileSystemFileHandle !== 'undefined' &&
    typeof FileSystemSyncAccessHandle !== 'undefined') {
  const now = () => performance.timeOrigin + performance.now();
  const handles = new WeakMap();
  const event = (op, slot = -1, begin = 0, end = 0, shared = 1) =>
    Module['_opfs_event'](op, slot, begin, end, shared);
  const file = FileSystemFileHandle.prototype;
  const access = FileSystemSyncAccessHandle.prototype;
  const originalOpen = file.createSyncAccessHandle;
  // Keep length == 0: the pinned SDK uses arity to detect an older browser API.
  file.createSyncAccessHandle = async function (...args) {
    const handle = await originalOpen.apply(this, args);
    const match = /^perf-([0-2])$/.exec(this.name);
    handles.set(handle, match ? Number(match[1]) : -1);
    event(4);
    return handle;
  };
  const originalBlob = file.getFile;
  file.getFile = async function (...args) {
    const blob = await originalBlob.apply(this, args);
    event(6);
    return blob;
  };
  for (const [name, op] of [['read', 1], ['write', 2], ['flush', 3], ['close', 5]]) {
    const original = access[name];
    access[name] = function (...args) {
      const slot = handles.get(this) ?? -1;
      const fault = Module['_opfs_take_fault'](op);
      const begin = now();
      try {
        if (fault === 1 || fault === 2) args[0] = args[0].subarray(0, 7);
        if (fault === 3) throw new TypeError('injected OPFS read argument error');
        if (fault === 4) throw new DOMException('injected OPFS quota error', 'QuotaExceededError');
        if (fault === 5) throw new DOMException('injected OPFS flush error', 'InvalidStateError');
        if (fault === 6) Module['_opfs_wait_gate']();
        return original.apply(this, args);
      } finally {
        const shared = op > 2 || (args[0].buffer === HEAPU8.buffer &&
          args[0].buffer instanceof SharedArrayBuffer);
        event(op, slot, begin, now(), Number(shared));
      }
    };
  }
  const directory = FileSystemDirectoryHandle.prototype;
  const originalDirectory = directory.getDirectoryHandle;
  directory.getDirectoryHandle = async function (...args) {
    if (args[0] === 'async-directory' && args[1]?.create && Module['_opfs_take_fault'](7)) {
      event(7);
      // Completion is driven by this owner's event loop while the C caller
      // blocks in WasmFS and holds a probe-owned mutex.
      await new Promise((resolve) => setTimeout(resolve, 150));
      const handle = await originalDirectory.apply(this, args);
      event(8);
      return handle;
    }
    return originalDirectory.apply(this, args);
  };
}
