/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
export const workerObserver = `addEventListener('message', event => {
  if (event.data?.postgammaSoakFault) {
    event.stopImmediatePropagation();
    const failure = new Error('Injected production control Worker failure');
    if (event.data.postgammaSoakFault === 1) setTimeout(() => {throw failure;}, 0);
    else setTimeout(() => {void Promise.reject(failure);}, 0);
    return;
  }
  if (!event.data?.postgammaSoakSnapshot) return;
  event.stopImmediatePropagation();
  const port = event.ports[0];
  Promise.resolve().then(() => globalThis.postgammaDiagnostic()).then(
    snapshot => port.postMessage({snapshot}), error => port.postMessage({error: String(error)})).finally(() => port.close());
}, {capture: true});\n`;
