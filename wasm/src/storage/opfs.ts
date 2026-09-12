/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
import {Namespace, encode, journalLimit, nodeLimit} from './namespace.js';
import type {Change, Node} from './namespace.js';

// This module runs only on the dedicated I/O owner. Calls are serialized there;
// it never enters PostgreSQL or waits for a kernel lock.
const utf8 = new TextEncoder();
const error = (name: string, message: string) => new DOMException(message, name);
export class Store {
  private root: any;
  private journal: any;
  private namespace = new Namespace();
  private bytes = 0;
  private poisoned = false;
  private handles = new Map<number, any>();
  private opens = new Map<number, number>();
  private removed = new Set<number>();
  private tail: Promise<unknown> = Promise.resolve();
  private channel: BroadcastChannel | undefined;
  private fault: {operation: string; path: string; action: string; remaining: number} | undefined;
  private resume: (() => void) | undefined;
  metrics = {reads: 0, writes: 0, flushes: 0, directoryFlushes: 0, namespaceCommits: 0,
    peakHandles: 0, discardedJournalBytes: 0, collectedOrphans: 0};
  async mount(name: string, testChannel: string) {
    if (!/^[a-zA-Z0-9_-]{1,64}$/.test(name)) throw error('TypeError', 'Invalid database name');
    const origin = await navigator.storage.getDirectory();
    const databases = await origin.getDirectoryHandle('postgamma', {create: true});
    this.root = await databases.getDirectoryHandle(name, {create: true});
    try {
      let journal;
      try {journal = await this.root.getFileHandle('namespace-v1');}
      catch (cause) {
        if ((cause as DOMException).name !== 'NotFoundError') throw cause;
        for await (const _entry of this.root.entries())
          throw error('NotFoundError', 'Namespace journal is missing from nonempty database storage');
        journal = await this.root.getFileHandle('namespace-v1', {create: true});
      }
      this.journal = await journal.createSyncAccessHandle();
      const length = this.journal.getSize();
      if (length > journalLimit) throw error('QuotaExceededError', 'Namespace journal exceeds its limit');
      const bytes = new Uint8Array(length);
      if (this.journal.read(bytes, {at: 0}) !== length) throw error('UnknownError', 'Short namespace read');
      const replay = Namespace.replay(bytes);
      this.namespace = replay.namespace; this.bytes = replay.validBytes;
      this.metrics.discardedJournalBytes = replay.discardedBytes;
      if (replay.discardedBytes) {this.journal.truncate(this.bytes); this.journal.flush();}
      // A published namespace must never silently create a missing data object.
      for (const node of this.namespace.nodes.values()) if (node.kind === 1)
        await this.root.getFileHandle('file-' + node.id);
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
          if (data.command === 'metrics') this.channel!.postMessage({metrics: this.metrics, request: data.request});
        };
        this.channel.postMessage({mounted: true, metrics: this.metrics});
      }
    } catch (cause) {this.journal?.close(); this.journal = undefined; throw cause;}
  }
  private path(id: number): string {
    const parts = [];
    for (let n = this.namespace.nodes.get(id); n && n.id !== 1; n = this.namespace.nodes.get(n.parent)) parts.push(n.name);
    return '/' + parts.reverse().join('/');
  }
  private async point(operation: string, id: number, path = this.path(id)): Promise<string> {
    const f = this.fault;
    if (!f || f.operation !== operation || !path.includes(f.path) || --f.remaining > 0) return '';
    this.fault = undefined;
    this.channel?.postMessage({gate: operation, path, action: f.action});
    if (f.action === 'pause') await new Promise<void>(resolve => {this.resume = resolve;});
    if (f.action === 'quota') throw error('QuotaExceededError', 'Injected OPFS quota failure');
    if (f.action === 'error') throw error('UnknownError', 'Injected OPFS failure');
    return f.action;
  }
  private async handle(id: number) {
    let h = this.handles.get(id);
    if (h) {this.handles.delete(id); this.handles.set(id, h); return h;}
    if (this.handles.size >= 64) {
      const [old, access] = this.handles.entries().next().value!;
      access.flush(); access.close(); this.handles.delete(old);
    }
    h = await (await this.root.getFileHandle('file-' + id)).createSyncAccessHandle();
    this.handles.set(id, h); this.metrics.peakHandles = Math.max(this.metrics.peakHandles, this.handles.size);
    return h;
  }
  private async collect(id: number) {
    if (!this.removed.has(id) || this.opens.has(id)) return;
    const h = this.handles.get(id);
    if (h) {h.close(); this.handles.delete(id);}
    try {await this.root.removeEntry('file-' + id); this.removed.delete(id);}
    catch { /* A failed deletion remains an orphan for the next owned mount. */ }
  }
  private async commit(change: Change) {
    const record = encode(change, this.namespace.sequence + 1);
    if (this.bytes + record.length > journalLimit) throw error('QuotaExceededError', 'Namespace journal is full');
    try {
      const action = await this.point('namespace-before', 0, change.op);
      if (action === 'partial') {
        const partial = record.subarray(0, Math.floor(record.length / 2));
        if (this.journal.write(partial, {at: this.bytes}) !== partial.length)
          throw error('UnknownError', 'Short injected journal tail');
        this.journal.flush();
        this.channel!.postMessage({gate: 'namespace-partial', path: change.op, bytes: partial.length});
        await new Promise<void>(resolve => {this.resume = resolve;});
        throw error('UnknownError', 'Interrupted namespace tail requires recovery');
      }
      let offset = 0;
      while (offset < record.length) {
        const n = this.journal.write(record.subarray(offset), {at: this.bytes + offset});
        if (n <= 0) throw error('UnknownError', 'Namespace write made no progress');
        offset += n;
      }
      await this.point('namespace-written', 0, change.op);
      this.journal.flush();
      await this.point('namespace-flushed', 0, change.op);
      this.namespace.apply(change); this.namespace.sequence++; this.bytes += record.length;
      this.metrics.namespaceCommits++;
    } catch (cause) {this.poisoned = true; throw cause;}
  }
  call(op: number, id: number, parent: number, name: string, data: Uint8Array, offset: number, mode: number): Promise<number> {
    const work = this.tail.then(() => this.execute(op, id, parent, name, data, offset, mode));
    this.tail = work.then(() => {}, () => {}); return work;
  }
  private async execute(op: number, id: number, parent: number, name: string, data: Uint8Array, offset: number, mode: number): Promise<number> {
    if (this.poisoned && op !== 12) throw error('UnknownError', 'OPFS namespace requires recovery');
    const view = () => new DataView(data.buffer, data.byteOffset, data.byteLength);
    const putNode = (node: Node) => {const v = view(); v.setUint32(0, node.id, true); v.setUint32(4, node.kind, true); v.setUint32(8, node.mode, true);};
    if (op === 1) {const node = this.namespace.lookup(id, name); if (!node) return 0; putNode(node); return node.id;}
    if (op === 2 || op === 3) {
      if (this.namespace.nodes.size >= nodeLimit) throw error('QuotaExceededError', 'Namespace node limit reached');
      const node = {id: this.namespace.nextId, parent: id, name, kind: op === 2 ? 1 : 2, mode};
      await this.point('create', id, this.path(id) + '/' + name);
      if (node.kind === 1) {
        const h = await (await this.root.getFileHandle('file-' + node.id, {create: true})).createSyncAccessHandle();
        try {h.truncate(0); h.flush();} finally {h.close();}
      }
      await this.commit({op: 'create', node}); putNode(node); return node.id;
    }
    if (op === 4 || op === 5) {
      const old = this.namespace.lookup(parent, name);
      await this.commit(op === 4 ? {op: 'move', id, parent, name} : {op: 'remove', parent, name});
      if (old?.kind === 1) {this.removed.add(old.id); await this.collect(old.id);}
      return 0;
    }
    if (op === 6) {
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
    if (op === 7) {await this.handle(id); this.opens.set(id, (this.opens.get(id) ?? 0) + 1); return 0;}
    if (op === 8) {
      const n = this.opens.get(id); if (!n) throw error('UnknownError', 'Unbalanced file close');
      if (n === 1) this.opens.delete(id); else this.opens.set(id, n - 1);
      await this.collect(id); return 0;
    }
    if (op === 12) {
      if (this.opens.size) throw error('InvalidStateError', 'Virtual files remain open');
      let failure: unknown;
      try {await this.point('close-flush', 1);} catch (cause) {failure = cause;}
      for (const h of this.handles.values()) {
        try {if (!this.poisoned) h.flush();} catch (cause) {failure ??= cause;}
        try {h.close();} catch (cause) {failure ??= cause;}
      }
      this.handles.clear();
      try {if (!this.poisoned) this.journal.flush();} catch (cause) {failure ??= cause;}
      try {this.journal.close();} catch (cause) {failure ??= cause;}
      this.journal = undefined;
      this.channel?.postMessage({closed: true, metrics: this.metrics, handles: this.handles.size, virtualOpens: this.opens.size,
        failed: failure !== undefined});
      this.channel?.close();
      if (failure) throw failure;
      return 0;
    }
    if (op === 13) {await this.point('directory-flush', id); this.journal.flush(); this.metrics.directoryFlushes++; return 0;}
    if (!Number.isSafeInteger(offset) || offset < 0) throw error('TypeError', 'Invalid file offset');
    const h = await this.handle(id);
    if (op === 9) {this.metrics.reads++; return h.read(data, {at: offset});}
    if (op === 10) {
      const action = await this.point('write', id); this.metrics.writes++;
      const n = h.write(action === 'short' ? data.subarray(0, Math.min(7, data.length)) : data, {at: offset});
      await this.point('written', id); return n;
    }
    if (op === 11) {await this.point('flush-before', id); h.flush(); this.metrics.flushes++; await this.point('flush-after', id); return 0;}
    if (op === 14) return h.getSize();
    if (op === 15) {await this.point('truncate', id); h.truncate(offset); return 0;}
    throw error('NotSupportedError', 'Unknown storage operation');
  }
}
