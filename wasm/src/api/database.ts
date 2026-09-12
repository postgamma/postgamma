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
  private pending = new Map<number, {resolve(value: ArrayBuffer): void; reject(error: Error): void}>();
  private failure: Error | null = null;
  metrics: unknown;
  private worker: Worker;
  constructor(worker: Worker) {
    this.worker = worker;
    worker.onmessage = ({data}) => {
      if (data.fatal) {this.fail(new Error(data.fatal)); return;}
      const waiter = this.pending.get(data.sequence);
      if (!waiter) return; // Retired sequence: never deliver it to a later request.
      this.pending.delete(data.sequence);
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
    const reply = await new Promise<ArrayBuffer>((resolve, reject) => {
      this.pending.set(sequence, {resolve, reject}); this.worker.postMessage(bytes, [bytes]);
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
  async query(sql: string, options: ExecuteOptions = {}): Promise<Result[]> {
    const request = await this.execute(sql, options), results: Result[] = [];
    try {for (;;) {const batch = await request.next(); if (batch === null) return results; results.push(batch);}}
    finally {await request.close();}
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
        const batch = availability === 2 ? null : result(r); r.end(); return batch;
      }
    } finally {this.reading = false;}
  }
  async cancel(): Promise<void> {(await this.transport.call(Op.cancel, this.handle)).end();}
  async close(): Promise<void> {(await this.transport.call(Op.release, this.handle)).end();}
}
