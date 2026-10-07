/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
import {pthreadPoolSize} from '../protocol/limits.js';
import {ownWorkerMailbox} from './messages.js';

// The API owner creates both the control Worker and the pthread Workers.
// Retiring a control Worker must not wait for nested browser Worker shutdown.
// Private ports preserve the SDK's Worker interface and transferable messages;
// native code, shared memory and OPFS handles remain in the original threads.
export function ownPthreads(control: Worker, moduleURL: URL, failed: (error: Error) => void,
  compiled?: WebAssembly.Module, memory?: WebAssembly.Memory): () => void {
  const channel = new MessageChannel(), group = crypto.randomUUID();
  const WorkerConstructor = Worker;
  const workers = new Map<number, {worker: Worker; port: MessagePort}>();
  const terminate = control.terminate;
  let closed = false;
  const retire = (id: number) => {
    const entry = workers.get(id);
    if (!entry) return;
    workers.delete(id);
    const {worker, port} = entry;
    worker.onmessage = null; worker.onerror = null; worker.onmessageerror = null;
    port.onmessage = null; port.onmessageerror = null;
    worker.terminate(); port.close();
  };
  const stop = () => {
    if (closed) return;
    closed = true;
    control.terminate = terminate;
    channel.port1.onmessage = null; channel.port1.onmessageerror = null;
    channel.port1.close();
    for (const id of workers.keys()) retire(id);
  };
  // Abrupt startup/recovery callers can terminate the captured control Worker
  // directly, before Database.open has returned a transport handle.
  control.terminate = () => {stop(); terminate.call(control);};
  channel.port1.onmessage = ({data, ports}) => {
    const port = ports[0];
    if (closed) {for (const item of ports) item.close(); return;}
    try {
      if (ports.length !== 1 || !Number.isSafeInteger(data?.id) || data.id <= 0 ||
          workers.has(data.id) || workers.size >= pthreadPoolSize ||
          data.options?.type !== 'module' || !/^em-pthread-\d+$/.test(data.options?.name))
        throw new Error('Invalid or excessive pthread Worker allocation');
      const url = new URL(data.url);
      if (url.href !== moduleURL.href) throw new Error('Pthread Worker module differs from the asset manifest');
      // Identity belongs to the Worker, not to the HTTP/compiled asset cache key.
      url.hash = new URLSearchParams({postgammaPthread: group + ':' + data.id}).toString();
      const worker = new WorkerConstructor(url, data.options);
      // Register the bootstrap consumer before any forwarding listener. Some
      // Worker targets invoke listeners in registration order despite capture;
      // forwarding the bootstrap would transfer its port away from this owner.
      const ready = ownWorkerMailbox(worker);
      const send = (value: unknown, transfer: Transferable[] = []) => {
        if (closed || !workers.has(data.id)) {
          for (const item of transfer) if (item instanceof MessagePort) item.close();
          return;
        }
        try {port.postMessage(value, transfer);}
        catch (cause) {retire(data.id); failed(new Error('Pthread Worker forwarding failed: ' + String(cause)));}
      };
      workers.set(data.id, {worker, port});
      worker.onmessage = event => send({event: 'message', data: event.data, ports: event.ports}, [...event.ports]);
      worker.onerror = event => {
        event.preventDefault();
        send({event: 'error', message: event.message, filename: event.filename, lineno: event.lineno, colno: event.colno});
      };
      worker.onmessageerror = () => send({event: 'messageerror'});
      void ready.catch(cause => {
        if (closed || !workers.has(data.id)) return;
        send({event: 'error', message: String(cause)});
        retire(data.id); failed(new Error('Pthread Worker mailbox failed: ' + String(cause)));
      });
      port.onmessage = ({data: message}) => {
        if (closed || !workers.has(data.id)) {
          if (Array.isArray(message?.transfer)) for (const item of message.transfer) if (item instanceof MessagePort) item.close();
          return;
        }
        try {
          if (message?.cmd === 'post' && Array.isArray(message.transfer)) {
            void ready.then(() => {
              if (closed || !workers.has(data.id)) {
                for (const item of message.transfer) if (item instanceof MessagePort) item.close();
                return;
              }
              try {worker.postMessage(message.data, message.transfer);}
              catch (cause) {retire(data.id); failed(new Error('Pthread Worker command failed: ' + String(cause)));}
            }, () => {for (const item of message.transfer) if (item instanceof MessagePort) item.close();});
          }
          else if (message?.cmd === 'terminate') retire(data.id);
          else throw new Error('Invalid pthread Worker command');
        } catch (cause) {retire(data.id); failed(new Error('Pthread Worker command failed: ' + String(cause)));}
      };
      port.onmessageerror = () => {
        if (closed || !workers.has(data.id)) return;
        retire(data.id); failed(new Error('Pthread Worker command could not be decoded'));
      };
    } catch (cause) {
      const error = new Error('Pthread Worker allocation failed: ' + String(cause));
      try {port?.postMessage({event: 'error', message: error.message});} catch { /* Control cleanup reports the failure. */ }
      for (const item of ports) item.close();
      failed(error);
    }
  };
  channel.port1.onmessageerror = () => failed(new Error('Pthread Worker allocation could not be decoded'));
  try {control.postMessage({postgammaPthreadBroker: true, compiled, memory}, [channel.port2]);}
  catch (cause) {channel.port2.close(); stop(); throw cause;}
  return stop;
}

// Installed before loadKernel preallocates the pinned SDK pool. Each facade
// owns one port; the API owner retains the real Worker until group retirement,
// even when a failed child closes its own scope before the fatal receipt.
export function installPthreadBroker(broker: MessagePort): void {
  let sequence = 0;
  class PthreadWorker extends EventTarget {
    readonly postgammaBrokerId: number;
    onmessage: Worker['onmessage'] = null;
    onmessageerror: Worker['onmessageerror'] = null;
    onerror: Worker['onerror'] = null;
    private port: MessagePort;
    private closed = false;
    constructor(url: string | URL, options?: WorkerOptions) {
      super();
      this.postgammaBrokerId = ++sequence;
      const channel = new MessageChannel(); this.port = channel.port1;
      this.port.onmessage = ({data}) => {
        if (this.closed) return;
        if (data.event === 'message') {
          const event = new MessageEvent('message', {data: data.data, ports: data.ports});
          this.dispatchEvent(event); this.onmessage?.call(this as unknown as Worker, event);
        } else if (data.event === 'error') {
          const event = new ErrorEvent('error', {cancelable: true, message: data.message,
            filename: data.filename, lineno: data.lineno, colno: data.colno});
          this.dispatchEvent(event); this.onerror?.call(this as unknown as Worker, event);
        } else {
          const event = new MessageEvent('messageerror');
          this.dispatchEvent(event); this.onmessageerror?.call(this as unknown as Worker, event);
          throw new Error('Pthread Worker message could not be decoded');
        }
      };
      this.port.onmessageerror = () => {throw new Error('Pthread Worker event could not be decoded');};
      try {broker.postMessage({id: this.postgammaBrokerId, url: String(url), options}, [channel.port2]);}
      catch (cause) {this.port.close(); channel.port2.close(); throw cause;}
    }
    postMessage(data: unknown, transferOrOptions: Transferable[] | StructuredSerializeOptions = []): void {
      if (this.closed) return;
      const transfer = Array.isArray(transferOrOptions) ? transferOrOptions : transferOrOptions.transfer ?? [];
      this.port.postMessage({cmd: 'post', data, transfer}, transfer);
    }
    terminate(): void {
      if (this.closed) return;
      this.closed = true;
      try {this.port.postMessage({cmd: 'terminate'});}
      finally {
        this.onmessage = null; this.onerror = null; this.onmessageerror = null;
        this.port.onmessage = null; this.port.onmessageerror = null; this.port.close();
      }
    }
  }
  globalThis.Worker = PthreadWorker as unknown as typeof Worker;
}
