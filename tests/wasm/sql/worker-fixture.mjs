/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
import assert from 'node:assert/strict';
// Browser-like Worker events with real MessagePorts. Calling the native owner
// postMessage is a test failure: bootstrap and commands must use the child port.
export class WorkerFixture extends EventTarget {
  terminated = 0; nativePosts = 0;
  captures = new Map();
  constructor(autoMailbox = true) {
    super();
    for (const type of ['message', 'messageerror', 'error']) {
      let callback = null;
      Object.defineProperty(this, 'on' + type, {configurable: true, get: () => callback, set: next => {
        if (callback) this.removeEventListener(type, callback);
        callback = next;
        if (callback) this.addEventListener(type, callback);
      }});
    }
    this.channel = new MessageChannel(); this.receivedPorts = new Set();
    this.channel.port1.onmessage = event => {
      for (const port of event.ports) this.receivedPorts.add(port);
      this.receive(event.data, event.ports);
    };
    if (autoMailbox) queueMicrotask(() => this.startMailbox());
  }
  startMailbox() {
    if (!this.terminated) this.dispatchEvent(new MessageEvent('message',
      {data: {postgammaWorkerMailbox: true}, ports: [this.channel.port2]}));
  }
  addEventListener(type, callback, options) {
    if (options === true || options?.capture) {
      const listeners = this.captures.get(type) ?? new Set();
      listeners.add(callback); this.captures.set(type, listeners);
    }
    super.addEventListener(type, callback, options);
  }
  removeEventListener(type, callback, options) {
    if (options === true || options?.capture) this.captures.get(type)?.delete(callback);
    super.removeEventListener(type, callback, options);
  }
  receive() {}
  emit(data, ports = []) {this.channel.port1.postMessage(data, ports);}
  postMessage() {this.nativePosts++; assert.fail('Owner called native Worker.postMessage');}
  terminate() {
    this.terminated++; this.channel.port1.close(); this.channel.port2.close();
    for (const port of this.receivedPorts) port.close(); this.receivedPorts.clear();
  }
}
export async function waitFor(predicate, message = 'Worker message progress') {
  const deadline = performance.now() + 2000;
  while (!predicate()) {
    assert(performance.now() < deadline, message);
    await new Promise(resolve => setImmediate(resolve));
  }
}
