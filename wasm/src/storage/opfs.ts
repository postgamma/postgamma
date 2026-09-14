/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
import {Namespace, nodeLimit} from './namespace.js';
import type {Change, Node} from './namespace.js';
import type {AccessHandle, StorageDirectory, StorageFile} from './files.js';
import {Journal} from './journal.js';
import {StorageOp} from './protocol.js';
import type {StorageOperation} from './protocol.js';
export {StorageOp, storageErrorCode} from './protocol.js';

export interface StorageRequest {
  id: number;
  parent: number;
  name: string;
  data: Uint8Array;
  offset: number;
  mode: number;
}

// This module runs only on the dedicated I/O owner. Calls are serialized there;
// it never enters PostgreSQL or waits for a kernel lock.
const utf8 = new TextEncoder();
const error = (name: string, message: string) => new DOMException(message, name);
export class Store {
  private root!: StorageDirectory;
  private journal: Journal | undefined;
  private namespace = new Namespace();
  private poisoned = false;
  private handles = new Map<number, AccessHandle>();
  private files = new Map<number, StorageFile>();
  // The exclusive I/O owner observes every mutation. Sizes remain valid across
  // handle eviction; querying the browser on every fstat is an IPC roundtrip.
  private sizes = new Map<number, number>();
  private dirty = new Set<number>();
  private opens = new Map<number, number>();
  private removed = new Set<number>();
  private tail: Promise<unknown> = Promise.resolve();
  private channel: BroadcastChannel | undefined;
  private fault: {operation: string; path: string; action: string; remaining: number} | undefined;
  private resume: (() => void) | undefined;
  metrics = {reads: 0, writes: 0, flushes: 0, directoryFlushes: 0, namespaceCommits: 0,
    peakHandles: 0, discardedJournalBytes: 0, collectedOrphans: 0,
    compactions: 0, journalBytes: 0, peakJournalBytes: 0, journalGeneration: 0};
  async mount(name: string, testChannel: string) {
    if (!/^[a-zA-Z0-9_-]{1,64}$/.test(name)) throw error('TypeError', 'Invalid database name');
    const origin = await navigator.storage.getDirectory() as unknown as StorageDirectory;
    const databases = await origin.getDirectoryHandle('postgamma', {create: true});
    this.root = await databases.getDirectoryHandle(name, {create: true});
    try {
      this.journal = await Journal.open(this.root, {
        point: (operation, path) => this.point(operation, 0, path),
        partial: async bytes => {
          this.channel!.postMessage({gate: 'namespace-partial', path: 'journal', bytes});
          await new Promise<void>(resolve => {this.resume = resolve;});
          throw error('UnknownError', 'Interrupted namespace tail requires recovery');
        },
        partialCheckpoint: testChannel ? () => this.fault?.operation === 'checkpoint-partial' : undefined,
      });
      this.namespace = this.journal.namespace;
      this.journalMetrics();
      // A published namespace must never silently create a missing data object.
      for (const node of this.namespace.nodes.values()) if (node.kind === 1)
        this.files.set(node.id, await this.root.getFileHandle('file-' + node.id));
      for await (const [entry] of this.root.entries()) {
        if (/^file-[0-9]+$/.test(entry) && !this.namespace.nodes.has(Number(entry.slice(5)))) {
          await this.root.removeEntry(entry); this.metrics.collectedOrphans++;
        }
      }
      if (testChannel) {
        this.channel = new BroadcastChannel(testChannel);
        this.channel.onmessage = ({data}) => {
          if (data.command === 'arm') {this.fault = data.fault; this.channel!.postMessage({armed: true, request: data.request});}
          if (data.command === 'release') {this.resume?.(); this.resume = undefined;}
          if (data.command === 'metrics') {
            this.journalMetrics();
            this.channel!.postMessage({metrics: this.metrics, request: data.request});
          }
          if (data.command === 'checkpoint') {
            // Test-channel work obeys the same serialization as native I/O.
            const work = this.tail.then(async () => {
              if (this.poisoned) throw error('UnknownError', 'OPFS namespace requires recovery');
              await this.journal!.checkpoint();
              this.journalMetrics();
              this.channel!.postMessage({checkpoint: true, request: data.request, metrics: this.metrics});
            });
            this.tail = work.catch(cause => {
              this.poisoned = true;
              this.channel!.postMessage({checkpoint: false, request: data.request, error: String(cause)});
            });
          }
        };
        this.channel.postMessage({mounted: true, metrics: this.metrics});
      }
    } catch (cause) {
      try {this.journal?.close();} catch { /* Preserve the mount failure. */ }
      this.journal = undefined;
      throw cause;
    }
  }
  private journalMetrics() {
    if (this.journal) Object.assign(this.metrics, this.journal.metrics);
  }
  private path(id: number): string {
    const parts = [];
    for (let n = this.namespace.nodes.get(id); n && n.id !== 1; n = this.namespace.nodes.get(n.parent)) parts.push(n.name);
    return '/' + parts.reverse().join('/');
  }
  private point(operation: string, id: number, path?: string): string | Promise<string> {
    const f = this.fault;
    if (!f || f.operation !== operation) return '';
    path ??= this.path(id);
    if (!path.includes(f.path) || --f.remaining > 0) return '';
    this.fault = undefined;
    this.channel?.postMessage({gate: operation, path, action: f.action});
    if (f.action === 'pause') return new Promise<string>(resolve => {this.resume = () => resolve(f.action);});
    if (f.action === 'quota') throw error('QuotaExceededError', 'Injected OPFS quota failure');
    if (f.action === 'error') throw error('UnknownError', 'Injected OPFS failure');
    return f.action;
  }
  private reserveHandle() {
    // Two metadata handles plus 63 data handles retain the total 65-handle
    // ceiling, including while a checkpoint replaces the previous log.
    if (this.handles.size >= 63) {
      const [old, access] = this.handles.entries().next().value!;
      if (this.dirty.has(old)) access.flush();
      access.close(); this.handles.delete(old); this.dirty.delete(old);
    }
  }
  private keepHandle(id: number, h: AccessHandle, size: number) {
    this.sizes.set(id, size);
    this.handles.set(id, h); this.metrics.peakHandles = Math.max(this.metrics.peakHandles, this.handles.size);
  }
  private async file(id: number, create = false) {
    let file = this.files.get(id);
    if (!file) {
      file = await this.root.getFileHandle('file-' + id, {create});
      this.files.set(id, file);
    }
    return file;
  }
  private async handle(id: number) {
    let h = this.handles.get(id);
    if (h) {
      if (!this.sizes.has(id)) this.sizes.set(id, h.getSize());
      this.handles.delete(id); this.handles.set(id, h); return h;
    }
    this.reserveHandle();
    h = await (await this.file(id)).createSyncAccessHandle();
    try {this.keepHandle(id, h, this.sizes.get(id) ?? h.getSize());}
    catch (cause) {h.close(); throw cause;}
    return h;
  }
  private async collect(id: number) {
    if (!this.removed.has(id) || this.opens.has(id)) return;
    const h = this.handles.get(id);
    if (h) {h.close(); this.handles.delete(id); this.dirty.delete(id);}
    this.sizes.delete(id); this.files.delete(id);
    try {await this.root.removeEntry('file-' + id); this.removed.delete(id);}
    catch { /* A failed deletion remains an orphan for the next owned mount. */ }
  }
  private async commit(change: Change) {
    try {
      await this.journal!.append(change);
      this.journalMetrics();
      this.metrics.namespaceCommits++;
    } catch (cause) {this.poisoned = true; throw cause;}
  }
  private async shutdown(): Promise<number> {
    if (this.opens.size) throw error('InvalidStateError', 'Virtual files remain open');
    let failure: unknown;
    try {const gate = this.point('close-flush', 1); if (gate) await gate;} catch (cause) {failure = cause;}
    // Preserve the first failure and still release every physical handle.
    // A poisoned journal is recovered on the next mount, without a new flush.
    for (const [id, h] of this.handles) {
      try {
        if (!this.poisoned && this.dirty.has(id)) h.flush();
      } catch (cause) {failure ??= cause;}
      try {
        h.close();
      } catch (cause) {failure ??= cause;}
    }
    this.handles.clear();
    this.files.clear();
    this.sizes.clear();
    this.dirty.clear();
    this.journalMetrics();
    try {
      if (!this.poisoned) this.journal!.flush();
    } catch (cause) {failure ??= cause;}
    try {this.journal!.close();} catch (cause) {failure ??= cause;}
    this.journal = undefined;
    this.channel?.postMessage({closed: true, metrics: this.metrics, handles: this.handles.size, virtualOpens: this.opens.size,
      failed: failure !== undefined});
    this.channel?.close();
    if (failure) throw failure;
    return 0;
  }
  call(op: StorageOperation, request: StorageRequest): Promise<number> {
    const work = this.tail.then(() => this.execute(op, request));
    this.tail = work.then(() => {}, () => {}); return work;
  }
  private async execute(op: StorageOperation, request: StorageRequest): Promise<number> {
    const {id, parent, name, data, offset, mode} = request;
    if (this.poisoned && op !== StorageOp.shutdown) throw error('UnknownError', 'OPFS namespace requires recovery');
    const view = () => new DataView(data.buffer, data.byteOffset, data.byteLength);
    const putNode = (node: Node) => {
      const v = view();
      v.setUint32(0, node.id, true);
      v.setUint32(4, node.kind, true);
      v.setUint32(8, node.mode, true);
    };
    if (op === StorageOp.lookup) {const node = this.namespace.lookup(id, name); if (!node) return 0; putNode(node); return node.id;}
    if (op === StorageOp.createFile || op === StorageOp.createDirectory) {
      if (this.namespace.nodes.size >= nodeLimit || this.namespace.nextId >= 0x80000000)
        throw error('QuotaExceededError', 'Namespace node or identity limit reached');
      const node = {id: this.namespace.nextId, parent: id, name, kind: op === StorageOp.createFile ? 1 : 2, mode};
      if (this.fault?.operation === 'create') {
        const gate = this.point('create', id, this.path(id) + '/' + name);
        if (gate) await gate;
      }
      if (node.kind === 1) {
        this.reserveHandle();
        const h = await (await this.file(node.id, true)).createSyncAccessHandle();
        try {h.truncate(0); h.flush(); this.keepHandle(node.id, h, 0);}
        catch (cause) {h.close(); throw cause;}
      }
      await this.commit({op: 'create', node}); putNode(node); return node.id;
    }
    if (op === StorageOp.move || op === StorageOp.remove) {
      const old = this.namespace.lookup(parent, name);
      await this.commit(op === StorageOp.move ? {op: 'move', id, parent, name} : {op: 'remove', parent, name});
      if (old?.kind === 1) {this.removed.add(old.id); await this.collect(old.id);}
      return 0;
    }
    if (op === StorageOp.list) {
      const children = this.namespace.children(id);
      const size = children.reduce((n, child) => n + 16 + utf8.encode(child.name).length, 0);
      if (!data.length) return size;
      if (data.length < size) throw error('UnknownError', 'Directory reply is too small');
      let at = 0; const v = view();
      for (const child of children) {
        const text = utf8.encode(child.name);
        [child.id, child.kind, child.mode, text.length].forEach((n, i) => v.setUint32(at + i * 4, n, true));
        data.set(text, at + 16); at += 16 + text.length;
      }
      return size;
    }
    // A virtual open pins identity. Acquire the bounded physical handle only
    // when data I/O needs it; mount/create have already validated the object.
    if (op === StorageOp.open) {
      if (!this.files.has(id)) throw error('NotFoundError', 'Unknown data object');
      this.opens.set(id, (this.opens.get(id) ?? 0) + 1); return 0;
    }
    if (op === StorageOp.close) {
      const n = this.opens.get(id); if (!n) throw error('UnknownError', 'Unbalanced file close');
      if (n === 1) this.opens.delete(id); else this.opens.set(id, n - 1);
      await this.collect(id); return 0;
    }
    if (op === StorageOp.shutdown) return this.shutdown();
    if (op === StorageOp.flushDirectory) {
      const gate = this.point('directory-flush', id); if (gate) await gate;
      this.journal!.flush(); this.metrics.directoryFlushes++; return 0;
    }
    if (!Number.isSafeInteger(offset) || offset < 0) throw error('TypeError', 'Invalid file offset');
    if (op === StorageOp.size && this.sizes.has(id)) return this.sizes.get(id)!;
    const h = await this.handle(id);
    if (op === StorageOp.read) {this.metrics.reads++; return h.read(data, {at: offset});}
    if (op === StorageOp.write) {
      let gate = this.point('write', id);
      const action = gate ? await gate : ''; this.metrics.writes++;
      let n: number;
      try {n = h.write(action === 'short' ? data.subarray(0, Math.min(7, data.length)) : data, {at: offset});}
      catch (cause) {this.sizes.delete(id); this.dirty.add(id); throw cause;}
      if (n > 0) {this.sizes.set(id, Math.max(this.sizes.get(id)!, offset + n)); this.dirty.add(id);}
      gate = this.point('written', id); if (gate) await gate; return n;
    }
    if (op === StorageOp.flush) {
      let gate = this.point('flush-before', id); if (gate) await gate;
      h.flush();
      this.dirty.delete(id);
      this.metrics.flushes++;
      gate = this.point('flush-after', id); if (gate) await gate;
      return 0;
    }
    if (op === StorageOp.size) return this.sizes.get(id)!;
    if (op === StorageOp.truncate) {
      const gate = this.point('truncate', id); if (gate) await gate;
      try {h.truncate(offset); this.sizes.set(id, offset);}
      catch (cause) {this.sizes.delete(id); throw cause;}
      finally {this.dirty.add(id);}
      return 0;
    }
    throw error('NotSupportedError', 'Unknown storage operation');
  }
}
