/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
// WebKit's WorkerUserGestureForwarder::create misses adoptRef: every Window
// Worker.postMessage leaks a refcounted object, even without a user gesture.
// Let each Worker transfer its mailbox to the owner instead. Database commands
// require no Window user gesture; MessagePort preserves their transfer semantics.
let installed = false;
export function installWorkerMailbox(): void {
  if (installed || typeof DedicatedWorkerGlobalScope === 'undefined' ||
      !(globalThis instanceof DedicatedWorkerGlobalScope)) return;
  installed = true;
  const scope = globalThis as unknown as DedicatedWorkerGlobalScope;
  const channel = new MessageChannel();
  channel.port1.onmessage = event => scope.dispatchEvent(new MessageEvent('message', {data: event.data, ports: [...event.ports]}));
  channel.port1.onmessageerror = () => scope.dispatchEvent(new MessageEvent('messageerror'));
  // This first message originates in the Worker, so it does not allocate the
  // leaking main-thread gesture forwarder. The owner sends nothing beforehand.
  scope.postMessage({postgammaWorkerMailbox: true}, [channel.port2]);
  scope.postMessage = channel.port1.postMessage.bind(channel.port1);
}

export function ownWorkerMailbox(worker: Worker): Promise<void> {
  return new Promise((resolve, reject) => {
    const terminate = worker.terminate;
    let port: MessagePort | undefined, settled = false, closed = false;
    const cleanup = () => {
      clearTimeout(timer);
      worker.removeEventListener('message', receive, {capture: true});
      worker.removeEventListener('error', failed, {capture: true});
      worker.removeEventListener('messageerror', unreadable, {capture: true});
    };
    const finish = (error?: Error) => {
      if (settled) return;
      settled = true; cleanup();
      if (error) {worker.terminate(); reject(error);} else resolve();
    };
    worker.terminate = () => {
      if (closed) return;
      closed = true;
      if (port) {port.onmessage = null; port.onmessageerror = null; port.close(); port = undefined;}
      worker.postMessage = () => {};
      terminate.call(worker);
      finish(new Error('Worker terminated before its mailbox was ready'));
    };
    const receive = (event: MessageEvent) => {
      event.stopImmediatePropagation();
      if (event.data?.postgammaWorkerMailbox !== true || event.ports.length !== 1) {
        for (const item of event.ports) item.close();
        finish(new Error('Invalid Worker mailbox handshake')); return;
      }
      port = event.ports[0];
      port.onmessage = event => {
        if (closed) {for (const item of event.ports) item.close(); return;}
        worker.dispatchEvent(new MessageEvent('message', {data: event.data, ports: [...event.ports]}));
      };
      port.onmessageerror = () => {if (!closed) worker.dispatchEvent(new MessageEvent('messageerror'));};
      worker.postMessage = port.postMessage.bind(port);
      finish();
    };
    const failed = (event: ErrorEvent) => {
      event.preventDefault(); event.stopImmediatePropagation();
      finish(new Error('Worker mailbox startup failed: ' + event.message));
    };
    const unreadable = (event: MessageEvent) => {
      event.stopImmediatePropagation(); finish(new Error('Worker mailbox handshake could not be decoded'));
    };
    const timer = setTimeout(() => finish(new Error('Worker mailbox startup exceeded its 30s deadline')), 30000);
    worker.addEventListener('message', receive, {capture: true});
    worker.addEventListener('error', failed, {capture: true});
    worker.addEventListener('messageerror', unreadable, {capture: true});
  });
}
