/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
let phase = 'MODULE_LOAD_FAILED';
try {
  const {default: createProbe} = await import('./deployment.mjs');
  phase = 'WASM_LOAD_FAILED';
  const response = await fetch(new URL('./deployment.wasm', import.meta.url));
  if (!response.ok) throw new Error(`Wasm asset returned HTTP ${response.status}`);
  const wasmBinary = new Uint8Array(await response.arrayBuffer());
  phase = 'WASM_COMPILE_FAILED';
  await WebAssembly.compile(wasmBinary);
  phase = 'RUNTIME_START_FAILED';
  const output = [];
  const module = await createProbe({wasmBinary,
    print: (text) => output.push(text), printErr: (text) => output.push(text)});
  const decode = (pointer) => JSON.parse(module.UTF8ToString(pointer));
  onmessage = async ({data}) => {
    try {
      let result;
      switch (data.command) {
        case 'start': result = {rc: module._deployment_start(data.round)}; break;
        case 'state': result = decode(module._deployment_state()); break;
        case 'release': module._deployment_release(); result = {}; break;
        case 'join': result = decode(module._deployment_join()); break;
        case 'opfs': result = await opfs(); break;
        default: throw new Error(`Unknown deployment command: ${data.command}`);
      }
      postMessage({id: data.id, result});
    } catch (error) { postMessage({id: data.id, error: String(error.stack || error)}); }
  };
  postMessage({kind: 'ready', worker: typeof document === 'undefined', isolated: crossOriginIsolated,
    sharedMemory: module.HEAPU8.buffer instanceof SharedArrayBuffer, heapBytes: module.HEAPU8.byteLength, output});
} catch (error) {
  postMessage({kind: 'rejected', code: phase, error: String(error.stack || error)});
}

async function opfs() {
  if (!navigator.storage?.getDirectory || typeof FileSystemFileHandle === 'undefined' ||
      !FileSystemFileHandle.prototype.createSyncAccessHandle)
    return {status: 'unsupported', scope: 'native-browser-api', reason: 'Sync access handle API unavailable'};
  let handle, root;
  const name = 'postgamma-deployment-probe';
  try {
    root = await navigator.storage.getDirectory();
    const file = await root.getFileHandle(name, {create: true});
    handle = await file.createSyncAccessHandle();
    const written = handle.write(new Uint8Array([23, 42, 99]), {at: 0});
    handle.flush();
    handle.close();
    handle = await file.createSyncAccessHandle();
    const data = new Uint8Array(3);
    const read = handle.read(data, {at: 0});
    return {status: written === 3 && read === 3 && data.join(',') === '23,42,99' ? 'passed' : 'failed',
      scope: 'native-browser-api', written, read, data: [...data], wasmfsValidated: false};
  } catch (error) { return {status: 'failed', scope: 'native-browser-api', error: String(error.stack || error)}; }
  finally {
    if (handle) handle.close();
    if (root) await root.removeEntry(name).catch(() => {});
  }
}
