/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
import {Op, Reader, VERSION, Writer, response} from '../protocol/wire.js';
import type {Handle} from '../protocol/wire.js';
import {result} from '../codecs/results.js';
import type {Result} from '../codecs/results.js';
export {DatabaseError} from '../protocol/wire.js';
export type {Result, Column} from '../codecs/results.js';
export interface Parameter {typeOid: number; value: string | Uint8Array | null; format?: 0 | 1}
export interface ExecuteOptions {parameters?: Parameter[]; binary?: boolean; chunkRows?: number}
const delay = () => new Promise<void>(resolve => setTimeout(resolve, 1));
class Transport {
  private sequence = 0;
  private pending = new Map<number, {bytes: number; resolve(value: ArrayBuffer): void; reject(error: Error): void}>();
  private failure: Error | null = null;
  private pendingBytes = 0;
  sample: {memory: number; threads: number; queuedBytes: number} | undefined;
  metrics: unknown;
  private worker: Worker;
  constructor(worker: Worker) {
    this.worker = worker;
    worker.onmessage = ({data}) => {
      if (data.fatal) {this.fail(new Error(data.fatal)); return;}
      const waiter = this.pending.get(data.sequence);
      if (!waiter) return; // Retired sequence: never deliver it to a later request.
      this.pending.delete(data.sequence); this.pendingBytes -= waiter.bytes;
      if (data.sample) this.sample = data.sample;
      if (data.metrics) this.metrics = data.metrics;
      waiter.resolve(data.reply);
    };
    worker.onerror = event => this.fail(new Error(event.message));
    worker.onmessageerror = () => this.fail(new Error('Worker message could not be decoded'));
  }
  private fail(error: Error) {
    this.failure = error;
    for (const waiter of this.pending.values()) waiter.reject(error);
    this.pending.clear(); this.worker.terminate();
  }
  async call(op: number, handle?: Handle, write?: (w: Writer) => void): Promise<Reader> {
    if (this.failure) throw this.failure;
    if (this.pending.size >= 64 || this.sequence === 0xffffffff) throw new Error('Request capacity exhausted');
    const sequence = ++this.sequence, w = new Writer().u32(VERSION).u32(sequence).u32(op);
    if (handle) w.handle(handle); write?.(w);
    const bytes = w.finish();
    if (this.pendingBytes + bytes.byteLength > 8 * 1024 * 1024) throw new Error('Worker byte queue is full');
    const reply = await new Promise<ArrayBuffer>((resolve, reject) => {
      this.pendingBytes += bytes.byteLength; this.pending.set(sequence, {resolve, reject, bytes: bytes.byteLength}); this.worker.postMessage(bytes, [bytes]);
    });
    return response(reply, sequence, op);
  }
  terminate() {this.worker.terminate();}
}
export class Database {
  private transport: Transport;
  private handle: Handle;
  private constructor(transport: Transport, handle: Handle) {this.transport = transport; this.handle = handle;}
  static async open(options: {create?: boolean; path?: string; workers?: number} = {}): Promise<Database> {
    const transport = new Transport(new Worker(new URL('../worker/sql.js', import.meta.url), {type: 'module'}));
    try {
      const r = await transport.call(Op.open, undefined, w => w.u32(options.create === false ? 0 : 1)
        .u32(options.workers ?? 4).text(options.path ?? '/database'));
      const handle = r.handle(); r.end(); return new Database(transport, handle);
    } catch (error) {transport.terminate(); throw error;}
  }
  async connect(name = 'postgamma-wasm'): Promise<Connection> {
    const r = await this.transport.call(Op.connect, this.handle, w => w.text(name));
    const h = r.handle(); r.end(); return new Connection(this.transport, h);
  }
  sample() {return this.transport.sample;}
  async telemetry() {
    const r = await this.transport.call(Op.telemetry, this.handle);
    const t = {workers: r.u32(), connections: r.u64(), requests: r.u64(), running: r.u64(),
      pinned: r.u64(), runnable: r.u64(), queued: r.u64(), rejections: r.u64()}; r.end(); return t;
  }
  async close(): Promise<unknown> {
    (await this.transport.call(Op.close, this.handle)).end();
    (await this.transport.call(Op.stop)).end(); this.transport.terminate(); return this.transport.metrics;
  }
}
export class Connection {
  private transport: Transport;
  private handle: Handle;
  constructor(transport: Transport, handle: Handle) {this.transport = transport; this.handle = handle;}
  async execute(sql: string, options: ExecuteOptions = {}): Promise<Request> {
    const r = await this.transport.call(Op.execute, this.handle, w => {
      w.text(sql).u32(options.binary ? 1 : 0).u32(options.chunkRows ?? 0);
      const parameters = options.parameters ?? []; w.u32(parameters.length);
      for (const p of parameters) w.u32(p.typeOid).u32(p.format ?? (p.value instanceof Uint8Array ? 1 : 0))
        .u32(p.value === null ? 1 : 0).blob(p.value === null ? new Uint8Array() :
          typeof p.value === 'string' ? new TextEncoder().encode(p.value) : p.value);
    });
    const h = r.handle(); r.end(); return new Request(this.transport, h);
  }
  private queryTail: Promise<unknown> = Promise.resolve();
  private queuedQueries = 0;
  query(sql: string, options: ExecuteOptions = {}): Promise<Result[]> {
    if (this.queuedQueries >= 64) return Promise.reject(new Error('Connection query queue is full'));
    this.queuedQueries++;
    const work = this.queryTail.then(async () => {
      const request = await this.execute(sql, options), results: Result[] = [];
      let bytes = 0;
      try {for (;;) {
        const batch = await request.next(); if (batch === null) return results;
        if (batch.copy) throw new Error('Use execute() for COPY streams');
        bytes += batch.byteLength;
        if (bytes > 4 * 1024 * 1024) throw new Error('Collected results exceed 4 MiB; consume an iterator');
        results.push(batch);
      }}
      finally {await request.close();}
    });
    this.queryTail = work.catch(() => {}).finally(() => this.queuedQueries--);
    return work;
  }
  async status() {
    const r = await this.transport.call(Op.status, this.handle);
    const value = {transaction: r.u32(), pinReasons: r.u32(), carrierRetained: r.u32(), pid: r.i32(),
      connectionId: r.u64(), requestId: r.u64()}; r.end(); return value;
  }
  async close(): Promise<void> {(await this.transport.call(Op.disconnect, this.handle)).end();}
}
export class Request {
  private reading = false;
  private copy: CopyStream | undefined;
  private transport: Transport;
  private handle: Handle;
  constructor(transport: Transport, handle: Handle) {this.transport = transport; this.handle = handle;}
  async next(): Promise<Result | null> {
    if (this.reading) throw new Error('Only one result consumer is allowed');
    this.reading = true;
    try {
      for (;;) {
        const r = await this.transport.call(Op.next, this.handle), availability = r.u32();
        if (availability === 1) {r.end(); await delay(); continue;}
        const batch = availability === 2 ? null : result(r);
        if (batch && (batch.kind === 2 || batch.kind === 3)) {
          this.copy = new CopyStream(this.transport, r.handle(), batch.kind === 2 ? 'in' : 'out');
          batch.copy = this.copy;
        }
        r.end(); return batch;
      }
    } finally {this.reading = false;}
  }
  async cancel(): Promise<void> {(await this.transport.call(Op.cancel, this.handle)).end();}
  async *[Symbol.asyncIterator](): AsyncGenerator<Result> {
    try {for (;;) {const batch = await this.next(); if (batch === null) return; yield batch;}}
    finally {await this.close();}
  }
  async close(): Promise<void> {
    if (this.copy) await this.copy.close();
    (await this.transport.call(Op.release, this.handle)).end();
  }
}

export class CopyStream {
  private transport: Transport;
  private handle: Handle;
  readonly direction: 'in' | 'out';
  private busy = false;
  private closed = false;
  private ended = false;
  constructor(transport: Transport, handle: Handle, direction: 'in' | 'out') {
    this.transport = transport; this.handle = handle; this.direction = direction;
  }
  private enter(direction: 'in' | 'out') {
    if (this.closed || this.busy || this.direction !== direction) throw new Error('COPY operation conflicts with stream state');
    this.busy = true;
  }
  async read(capacity = 16384): Promise<Uint8Array | null> {
    this.enter('out');
    try {
      if (this.ended) return null;
      for (;;) {
        const r = await this.transport.call(Op.copyRead, this.handle, w => w.u32(capacity));
        const state = r.u32(), bytes = r.blob(); r.end();
        if (state === 2) {this.ended = true; return null;}
        if (bytes?.length) return bytes;
        await delay();
      }
    } finally {this.busy = false;}
  }
  async write(bytes: Uint8Array): Promise<void> {
    this.enter('in');
    try {
      if (this.ended) throw new Error('COPY input has ended');
      let offset = 0;
      while (offset < bytes.length) {
        const piece = bytes.subarray(offset, offset + 16384);
        const r = await this.transport.call(Op.copyWrite, this.handle, w => w.blob(piece));
        const state = r.u32(), consumed = r.u32(); r.end();
        if (consumed > piece.length) throw new Error('Invalid COPY consumption count');
        offset += consumed;
        if (state === 2 && offset < bytes.length) throw new Error('COPY ended before all input was consumed');
        if (state === 1 || !consumed) await delay();
      }
    } finally {this.busy = false;}
  }
  async finish(): Promise<void> {
    this.enter('in');
    try {
      if (this.ended) return;
      for (;;) {
        const r = await this.transport.call(Op.copyFinish, this.handle), state = r.u32(); r.end();
        if (state === 2) {this.ended = true; return;}
        await delay();
      }
    } finally {this.busy = false;}
  }
  async abort(): Promise<void> {(await this.transport.call(Op.copyAbort, this.handle)).end();}
  async close(): Promise<void> {
    if (this.closed) return;
    (await this.transport.call(Op.copyClose, this.handle)).end(); this.closed = true;
  }
  async *[Symbol.asyncIterator](): AsyncGenerator<Uint8Array> {
    try {for (;;) {const bytes = await this.read(); if (bytes === null) return; yield bytes;}}
    finally {await this.close();}
  }
}
