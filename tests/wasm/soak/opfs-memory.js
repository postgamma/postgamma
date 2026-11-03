/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
import {Store, StorageOp} from './src/storage/opfs.js';

self.onmessage = async ({data: parameters}) => {
  if (!parameters.name) return;
  try {
    const store = new Store(); globalThis.postgammaStore = store;
    await store.mount(parameters.name, '');
    const packet = new Uint8Array(16), payload = new Uint8Array(8192); payload.fill(73);
    const request = (id = 0, name = '', data = packet) => ({id, parent: 1, name, data, offset: 0, mode: 448});
    const call = (op, id = 0, name = '', data = packet) => store.call(op, request(id, name, data));
    let fixed;
    if (parameters.workload === 'opfs-reuse') {
      fixed = await call(StorageOp.createFile, 1, 'temporary');
      await call(StorageOp.open, fixed);
    }
    for (let iteration = 0; iteration <= parameters.iterations; iteration++) {
      if (iteration) {
        const id = fixed ?? await call(StorageOp.createFile, 1, 'temporary');
        if (!fixed) await call(StorageOp.open, id);
        await call(StorageOp.write, id, '', payload);
        if (!fixed) {
          await call(StorageOp.close, id);
          await call(StorageOp.remove, 0, 'temporary');
        }
      }
      if (iteration % 100 === 0 || iteration === parameters.iterations) {
        postMessage({sample: true, iteration, snapshot: {...store.diagnostics(),
          files: store.files.size, sizes: store.sizes.size, synced: store.synced.size,
          namespaceNodes: store.namespace.nodes.size}});
        await new Promise(resolve => {self.onmessage = ({data}) => {if (data.resume) resolve();};});
      }
    }
    if (fixed) {
      await call(StorageOp.flush, fixed);
      await call(StorageOp.close, fixed);
      await call(StorageOp.remove, 0, 'temporary');
    }
    await call(StorageOp.shutdown);
    postMessage({done: true, snapshot: store.diagnostics()}); self.close();
  } catch (error) {postMessage({error: String(error.stack || error)}); self.close();}
};
