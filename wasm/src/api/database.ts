/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
import {DatabaseError, Op, Reader, VERSION, Writer, response} from '../protocol/wire.js';
import type {Handle} from '../protocol/wire.js';
import {result} from '../codecs/results.js';
import type {Result} from '../codecs/results.js';
export {DatabaseError} from '../protocol/wire.js';
export type {Result, Column} from '../codecs/results.js';
import {limits} from '../protocol/limits.js';
import {assets, verifiedAsset} from '../loader/assets.js';
import {Readiness} from '../protocol/readiness.js';
export interface OpenOptions {
  create?: boolean; path?: string; workers?: number;
  storage?: {type: 'opfs'; name: string};
  /** URL of the complete matching asset directory, ending with a slash. */
  assetBase?: string | URL;
  /** Optional periodic diagnostics, 1..60000 ms; default 0 samples on commands only. */
  diagnostics?: {sampleIntervalMs?: number};
}
export class ClosedError extends Error {constructor() {super('Database handle is closing or closed'); this.name = 'ClosedError';}}
export interface Parameter {typeOid: number; value: string | Uint8Array | null; format?: 0 | 1}
export interface ExecuteOptions {parameters?: Parameter[]; binary?: boolean; chunkRows?: number}
class Transport {
  readonly readiness = new Readiness();
  private sequence = 0;
  private pending = new Map<number, {bytes: number; resolve(value: ArrayBuffer): void; reject(error: Error): void}>();
  private failure: Error | null = null;
  private failureWork: Promise<void> | undefined;
  private finishRetirement: (() => void) | undefined;
  private retirementTimer: ReturnType<typeof setTimeout> | undefined;
  private retired = false;
  private pendingBytes = 0;
  sample: {memory: number; threads: number; queuedBytes: number; periodicSamples: number; driverWakeCount: number} | undefined;
  metrics: unknown;
  peakHostQueuedBytes = 0;
  hostQueuedBytes = 0;
  private worker: Worker;
  constructor(worker: Worker) {
    this.worker = worker;
    worker.onmessage = ({data}) => {
      if (data.metrics) this.metrics = data.metrics;
      if (data.fatal) {this.retire(new Error(data.fatal)); return;}
      if (this.failure) return;
      if (data.ready) {this.readiness.notify(data.ready); return;}
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
  private retire(error: Error) {
    if (this.retired) return;
    this.retired = true;
    clearTimeout(this.retirementTimer);
    if (this.failure && this.failure !== error) this.failure.cause = error;
    this.failure ??= error;
    this.readiness.fail(this.failure);
    for (const waiter of this.pending.values()) waiter.reject(this.failure);
    this.pending.clear(); this.pendingBytes = 0; this.worker.terminate();
    this.finishRetirement?.();
  }
  private fail(error: Error): Promise<void> {
    if (this.failureWork) return this.failureWork;
    if (this.retired) return Promise.resolve();
    this.failure = error;
    this.failureWork = new Promise(resolve => {this.finishRetirement = resolve;});
    // A control Worker can release OPFS even when its native module failed.
    // Stop admission now, but reject callers only after its cleanup reply.
    this.retirementTimer = setTimeout(() => {
      error.message += '; control Worker cleanup acknowledgement timed out';
      this.retire(error);
    }, 10000);
    try {this.worker.postMessage({discard: String(error)});}
    catch (cause) {error.cause = cause; this.retire(error);}
    return this.failureWork;
  }
  async call(op: number, handle?: Handle, write?: (w: Writer) => void): Promise<Reader> {
    if (this.failure) {await this.failureWork; throw this.failure;}
    if (this.pending.size >= limits.maxPendingCommands || this.sequence === 0xffffffff) throw new Error('Request capacity exhausted');
    const sequence = ++this.sequence, w = new Writer().u32(VERSION).u32(sequence).u32(op);
    if (handle) w.handle(handle); write?.(w);
    const bytes = w.finish();
    if (this.pendingBytes + bytes.byteLength > limits.workerQueueBytes) throw new Error('Worker byte queue is full');
    const reply = await new Promise<ArrayBuffer>((resolve, reject) => {
      this.pendingBytes += bytes.byteLength; this.pending.set(sequence, {resolve, reject, bytes: bytes.byteLength}); this.worker.postMessage(bytes, [bytes]);
    });
    try {return response(reply, sequence, op);}
    catch (error) {
      if (!(error instanceof DatabaseError) || error.status === 8) await this.fail(error as Error);
      throw error;
    }
  }
  terminate() {this.retire(new ClosedError());}
}
export class Database {
  private transport: Transport;
  private handle: Handle;
  private connections = new Set<Connection>();
  private starting = new Set<Promise<Connection>>();
  private closing = false;
  private nativeClosed = false;
  private closeWork: Promise<unknown> | undefined;
  private constructor(transport: Transport, handle: Handle) {this.transport = transport; this.handle = handle;}
  static async open(options: OpenOptions = {}): Promise<Database> {
    const interval = options.diagnostics?.sampleIntervalMs ?? 0;
    if (!Number.isInteger(interval) || interval < 0 || interval > 60000)
      throw new RangeError('sampleIntervalMs must be an integer from 0 to 60000');
    const base = options.assetBase ? new URL(options.assetBase, globalThis.location.href) : new URL('../../', import.meta.url);
    if (!base.pathname.endsWith('/')) throw new Error('assetBase must end with a slash');
    const manifest = await assets(base);
    if (manifest.protocolVersion !== VERSION) throw new Error('postgamma bridge protocol mismatch');
    await verifiedAsset(base, manifest, manifest.entries.worker);
    const transport = new Transport(new Worker(new URL(manifest.entries.worker, base), {type: 'module'}));
    try {
      const r = await transport.call(Op.open, undefined, w => w.u32(options.create === false ? 0 : 1)
        .u32(options.workers ?? 4).text(options.path ?? '/database')
        .u32(options.storage ? 1 : 0).text(options.storage?.name ?? '')
        .text((options as {storageTestChannel?: string}).storageTestChannel ?? '').u32(interval));
      const handle = r.handle(); r.end(); return new Database(transport, handle);
    } catch (error) {transport.terminate(); throw error;}
  }
  async connect(name = 'postgamma-wasm'): Promise<Connection> {
    if (this.closing) throw new ClosedError();
    const work = this.transport.call(Op.connect, this.handle, w => w.text(name)).then(r => {
      const h = r.handle(); r.end();
      const connection = new Connection(this.transport, h, () => this.connections.delete(connection));
      this.connections.add(connection); return connection;
    });
    this.starting.add(work);
    try {return await work;} finally {this.starting.delete(work);}
  }
  diagnostics() {return this.transport.metrics;}
  sample() {return this.transport.sample;}
  async telemetry() {
    const r = await this.transport.call(Op.telemetry, this.handle);
    const t = {workers: r.u32(), connections: r.u64(), requests: r.u64(), running: r.u64(),
      pinned: r.u64(), runnable: r.u64(), queued: r.u64(), rejections: r.u64()}; r.end(); return t;
  }
  close(): Promise<unknown> {
    if (this.closeWork) return this.closeWork;
    this.closing = true;
    this.closeWork = (async () => {
      await Promise.allSettled(this.starting);
      await Promise.all([...this.connections].map(connection => connection.close()));
      const r = await this.transport.call(Op.close, this.handle);
      // A successful native close frees the instance. Its handle cannot be
      // retried, even if resource auditing or Worker teardown subsequently fails.
      this.nativeClosed = true;
      const failures: unknown[] = [];
      let resources: Record<string, number> | undefined;
      try {
        resources = {locks: r.u32(), shmem: r.u32(), dsm: r.u32(), pipes: r.u32()}; r.end();
        if (Object.values(resources).some(count => count !== 0)) throw new Error('Native resources remain after close');
      } catch (error) {failures.push(error);}
      try {(await this.transport.call(Op.stop)).end();}
      catch (error) {failures.push(error);}
      finally {this.transport.terminate();}
      this.transport.metrics = {...this.transport.metrics as object, resources,
        peakHostQueuedBytes: this.transport.peakHostQueuedBytes};
      if (failures.length === 1) throw failures[0];
      if (failures.length) throw new AggregateError(failures, 'Database close and teardown failed');
      return this.transport.metrics;
    })().catch(error => {if (!this.nativeClosed) this.closeWork = undefined; throw error;});
    return this.closeWork;
  }
}
export class Connection {
  private transport: Transport;
  private handle: Handle;
  private requests = new Set<Request>();
  private starting = new Set<Promise<Request>>();
  private closing = false;
  private closeWork: Promise<void> | undefined;
  private onClosed: () => void;
  constructor(transport: Transport, handle: Handle, onClosed: () => void) {
    this.transport = transport; this.handle = handle; this.onClosed = onClosed;
  }
  async execute(sql: string, options: ExecuteOptions = {}): Promise<Request> {
    if (this.closing) throw new ClosedError();
    const work = this.transport.call(Op.execute, this.handle, w => {
      w.text(sql).u32(options.binary ? 1 : 0).u32(options.chunkRows ?? 0);
      const parameters = options.parameters ?? []; w.u32(parameters.length);
      for (const p of parameters) w.u32(p.typeOid).u32(p.format ?? (p.value instanceof Uint8Array ? 1 : 0))
        .u32(p.value === null ? 1 : 0).blob(p.value === null ? new Uint8Array() :
          typeof p.value === 'string' ? new TextEncoder().encode(p.value) : p.value);
    }).then(r => {
      const h = r.handle(); r.end();
      const request = new Request(this.transport, h, () => this.requests.delete(request));
      this.requests.add(request); return request;
    });
    this.starting.add(work);
    try {return await work;} finally {this.starting.delete(work);}
  }
  private queryTail: Promise<unknown> = Promise.resolve();
  private queuedQueries = 0;
  private queuedBytes = 0;
  query(sql: string, options: ExecuteOptions = {}): Promise<Result[]> {
    if (this.closing) return Promise.reject(new ClosedError());
    if (this.queuedQueries >= limits.maxPendingCommands) return Promise.reject(new Error('Connection query queue is full'));
    let bytes = sql.length * 3 + 64;
    for (const p of options.parameters ?? [])
      bytes += 16 + (typeof p.value === 'string' ? p.value.length * 3 : p.value?.byteLength ?? 0);
    if (bytes > limits.commandBytes || this.queuedBytes + bytes > limits.connectionQueueBytes)
      return Promise.reject(new Error('Connection query byte queue is full'));
    this.queuedBytes += bytes; this.transport.hostQueuedBytes += bytes;
    this.transport.peakHostQueuedBytes = Math.max(this.transport.peakHostQueuedBytes, this.transport.hostQueuedBytes);
    this.queuedQueries++;
    const work = this.queryTail.then(async () => {
      const request = await this.execute(sql, options), results: Result[] = [];
      let bytes = 0;
      try {for (;;) {
        const batch = await request.next(); if (batch === null) return results;
        if (batch.copy) throw new Error('Use execute() for COPY streams');
        bytes += batch.byteLength;
        if (bytes > limits.collectedResultBytes) throw new Error('Collected results exceed 4 MiB; consume an iterator');
        results.push(batch);
      }}
      finally {await request.close();}
    });
    this.queryTail = work.then(() => {}, () => {}).finally(() => {this.queuedQueries--; this.queuedBytes -= bytes; this.transport.hostQueuedBytes -= bytes;});
    return work;
  }
  async status() {
    const r = await this.transport.call(Op.status, this.handle);
    const value = {transaction: r.u32(), pinReasons: r.u32(), carrierRetained: r.u32(), pid: r.i32(),
      connectionId: r.u64(), requestId: r.u64()}; r.end(); return value;
  }
  close(): Promise<void> {
    if (this.closeWork) return this.closeWork;
    this.closing = true;
    this.closeWork = (async () => {
      await Promise.allSettled(this.starting);
      await Promise.all([...this.requests].map(request => request.close()));
      await this.queryTail;
      (await this.transport.call(Op.disconnect, this.handle)).end(); this.onClosed();
    })().catch(error => {this.closeWork = undefined; throw error;});
    return this.closeWork;
  }
}
export class Request {
  private reading = false;
  private readDone?: Promise<void>;
  private finishRead?: () => void;
  private stopped = new AbortController();
  private copy: CopyStream | undefined;
  private transport: Transport;
  private handle: Handle;
  private closing = false;
  private closeWork: Promise<void> | undefined;
  private onClosed: () => void;
  constructor(transport: Transport, handle: Handle, onClosed: () => void) {
    this.transport = transport; this.handle = handle; this.onClosed = onClosed;
    this.transport.readiness.track(handle);
  }
  async next(): Promise<Result | null> {
    if (this.reading) throw new Error('Only one result consumer is allowed');
    this.reading = true;
    this.readDone = new Promise(resolve => {this.finishRead = resolve;});
    try {
      for (;;) {
        if (this.closing) throw new ClosedError();
        const version = this.transport.readiness.version(this.handle);
        const r = await this.transport.call(Op.next, this.handle), availability = r.u32();
        if (availability === 1) {
          r.end(); await this.transport.readiness.wait(this.handle, version, this.stopped.signal); continue;
        }
        const batch = availability === 2 ? null : result(r);
        if (batch && (batch.kind === 2 || batch.kind === 3)) {
          this.copy = new CopyStream(this.transport, r.handle(), batch.kind === 2 ? 'in' : 'out');
          batch.copy = this.copy;
        }
        r.end(); return batch;
      }
    } finally {this.reading = false; this.finishRead?.();}
  }
  async cancel(): Promise<void> {(await this.transport.call(Op.cancel, this.handle)).end();}
  async *[Symbol.asyncIterator](): AsyncGenerator<Result> {
    try {for (;;) {const batch = await this.next(); if (batch === null) return; yield batch;}}
    finally {await this.close();}
  }
  close(): Promise<void> {
    if (this.closeWork) return this.closeWork;
    this.closing = true;
    this.stopped.abort(new ClosedError());
    this.closeWork = (async () => {
      await this.readDone;
      if (this.copy) await this.copy.close();
      (await this.transport.call(Op.release, this.handle)).end();
      this.transport.readiness.retire(this.handle); this.onClosed();
    })().catch(error => {this.closeWork = undefined; throw error;});
    return this.closeWork;
  }
}

export class CopyStream {
  private transport: Transport;
  private handle: Handle;
  readonly direction: 'in' | 'out';
  private busy = false;
  private idle?: Promise<void>;
  private finishOperation?: () => void;
  private stopped = new AbortController();
  private closed = false;
  private closeWork: Promise<void> | undefined;
  private ended = false;
  constructor(transport: Transport, handle: Handle, direction: 'in' | 'out') {
    this.transport = transport; this.handle = handle; this.direction = direction;
    this.transport.readiness.track(handle);
  }
  private enter(direction: 'in' | 'out') {
    if (this.closed || this.busy || this.direction !== direction) throw new Error('COPY operation conflicts with stream state');
    this.busy = true;
    this.idle = new Promise(resolve => {this.finishOperation = resolve;});
  }
  async read(capacity = limits.copyChunkBytes): Promise<Uint8Array | null> {
    this.enter('out');
    try {
      if (this.ended) return null;
      for (;;) {
        if (this.closed) throw new ClosedError();
        const version = this.transport.readiness.version(this.handle);
        const r = await this.transport.call(Op.copyRead, this.handle, w => w.u32(capacity));
        const state = r.u32(), bytes = r.blob(); r.end();
        if (state === 2) {this.ended = true; return null;}
        if (bytes?.length) return bytes;
        if (state === 1) await this.transport.readiness.wait(this.handle, version, this.stopped.signal);
        else throw new Error('COPY read made no progress');
      }
    } finally {this.busy = false; this.finishOperation?.();}
  }
  async write(bytes: Uint8Array): Promise<void> {
    this.enter('in');
    try {
      if (this.ended) throw new Error('COPY input has ended');
      let offset = 0;
      while (offset < bytes.length) {
        if (this.closed) throw new ClosedError();
        const version = this.transport.readiness.version(this.handle);
        const piece = bytes.subarray(offset, offset + limits.copyChunkBytes);
        const r = await this.transport.call(Op.copyWrite, this.handle, w => w.blob(piece));
        const state = r.u32(), consumed = r.u32(); r.end();
        if (consumed > piece.length) throw new Error('Invalid COPY consumption count');
        offset += consumed;
        if (state === 2 && offset < bytes.length) throw new Error('COPY ended before all input was consumed');
        if (state === 1 && offset < bytes.length)
          await this.transport.readiness.wait(this.handle, version, this.stopped.signal);
        else if (!consumed) throw new Error('COPY write made no progress');
      }
    } finally {this.busy = false; this.finishOperation?.();}
  }
  async finish(): Promise<void> {
    this.enter('in');
    try {
      if (this.ended) return;
      for (;;) {
        if (this.closed) throw new ClosedError();
        const version = this.transport.readiness.version(this.handle);
        const r = await this.transport.call(Op.copyFinish, this.handle), state = r.u32(); r.end();
        if (state === 2) {this.ended = true; return;}
        if (state === 1) await this.transport.readiness.wait(this.handle, version, this.stopped.signal);
      }
    } finally {this.busy = false; this.finishOperation?.();}
  }
  async abort(): Promise<void> {(await this.transport.call(Op.copyAbort, this.handle)).end();}
  close(): Promise<void> {
    if (this.closeWork) return this.closeWork;
    this.closed = true;
    this.stopped.abort(new ClosedError());
    this.closeWork = (async () => {
      await this.idle;
      (await this.transport.call(Op.copyClose, this.handle)).end();
      this.transport.readiness.retire(this.handle);
    })().catch(error => {this.closeWork = undefined; throw error;});
    return this.closeWork;
  }
  async *[Symbol.asyncIterator](): AsyncGenerator<Uint8Array> {
    try {for (;;) {const bytes = await this.read(); if (bytes === null) return; yield bytes;}}
    finally {await this.close();}
  }
}
