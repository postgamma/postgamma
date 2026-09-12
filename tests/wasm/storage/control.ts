/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
export class Control {
  private channel: BroadcastChannel;
  private events: any[] = [];
  private listeners = new Set<() => void>();
  private identity = crypto.randomUUID();
  private sequence = 0;
  constructor(name: string) {
    this.channel = new BroadcastChannel(name);
    this.channel.onmessage = ({data}) => {
      this.events.push(data);
      (window as any).probeEvents.push({kind: 'storage-control', ...data});
      for (const listener of this.listeners) listener();
    };
  }
  wait(predicate: (event: any) => boolean, timeoutMs = 5000): Promise<any> {
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => {this.listeners.delete(check); reject(new Error('Storage control event timed out'));}, timeoutMs);
      const check = () => {
        const i = this.events.findIndex(predicate);
        if (i < 0) return;
        clearTimeout(timer); this.listeners.delete(check); resolve(this.events.splice(i, 1)[0]);
      };
      this.listeners.add(check); check();
    });
  }
  async arm(operation: string, path: string, action = 'pause', remaining = 1) {
    const request = this.identity + ':' + ++this.sequence;
    const ready = this.wait(event => event.armed && event.request === request);
    this.channel.postMessage({command: 'arm', request, fault: {operation, path, action, remaining}}); await ready;
  }
  release() {this.channel.postMessage({command: 'release'});}
  async metrics() {
    return (await this.inspect()).metrics;
  }
  async inspect() {
    const request = this.identity + ':' + ++this.sequence;
    const ready = this.wait(event => event.metrics && event.request === request);
    this.channel.postMessage({command: 'metrics', request}); return await ready;
  }
  close() {this.channel.close();}
}
