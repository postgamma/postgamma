/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
import {DatabaseError, Op, Reader, VERSION, Writer, response, utf8Length} from '../protocol/wire.js';
import type {Handle} from '../protocol/wire.js';
import {result} from '../codecs/results.js';
import type {Result} from '../codecs/results.js';
export {DatabaseError} from '../protocol/wire.js';
export type {Result, Column} from '../codecs/results.js';
import {limits} from '../protocol/limits.js';
import {assets, verifiedAsset} from '../loader/assets.js';
import {compiledKernel} from '../loader/compiled.js';
import {ownedMemory} from '../loader/memory.js';
import {Readiness} from '../protocol/readiness.js';
import {buildId} from '../loader/identity.js';
import {ArchiveEncoder, ArchiveDecoder} from '../storage/archive.js';
import {ownPthreads} from '../runtime/pthreads.js';
import {ownWorkerMailbox} from '../runtime/messages.js';
export interface ArchiveOptions {signal?: AbortSignal}
export type RestoreOptions = Omit<OpenOptions, 'create'> & ArchiveOptions;
const archiveHeader = () => new TextEncoder().encode(JSON.stringify({format: 'postgamma.archive', version: 1, buildId}));
function openMessage(w: Writer, options: OpenOptions) {
  w.u32(options.create === false ? 0 : 1).u32(options.workers ?? 4).text(options.path ?? '/database')
    .u32(options.storage ? 1 : 0).text(options.storage?.name ?? '')
    .text((options as {storageTestChannel?: string}).storageTestChannel ?? '')
    .u32(options.diagnostics?.sampleIntervalMs ?? 0);
}
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
export interface ScriptOptions {chunkRows?: number}
export interface Notification {channel: string; payload: string; pid: number}
export class NotificationOverflowError extends Error {
  readonly dropped: bigint;
  constructor(dropped: bigint) {super(`Native notification queue dropped ${dropped} events`); this.name = 'NotificationOverflowError'; this.dropped = dropped;}
}
type NotificationHandler = {notify(value: Notification): void | Promise<void>; error?: (error: unknown) => void};
const handleKey = (h: Handle) => `${h.id}:${h.generation}`;
// Native startup has a 60s control deadline and a 30s handshake deadline.
// If its ticket or aborted backend cannot retire, discard the whole instance:
// returning a connection error alone would leave native payload owners alive.
const connectionStartupTimeoutMs = 90000;
function notificationError(handler: NotificationHandler, error: unknown) {
  try {if (handler.error) handler.error(error); else console.error(error);}
  catch (cause) {console.error(cause);}
}
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
  private worker: Worker | undefined;
  private stopPthreads: () => void;
  private notificationHandlers = new Map<string, Set<NotificationHandler>>();
  private eventsPending = false;
  private eventsRunning = false;
  private eventsEnabled = true;
  constructor(worker: Worker, moduleURL: URL, compiled: WebAssembly.Module, memory: WebAssembly.Memory) {
    this.worker = worker;
    worker.onmessage = ({data}) => {
      if (data.metrics) this.metrics = data.metrics;
      if (data.fatal) {this.retire(new Error(data.fatal)); return;}
      if (this.failure) return;
      if (data.notificationsReady) {
        if (this.eventsEnabled) {this.eventsPending = true; void this.events();}
        return;
      }
      if (data.ready) {this.readiness.notify(data.ready); return;}
      const waiter = this.pending.get(data.sequence);
      if (!waiter) return; // Retired sequence: never deliver it to a later request.
      this.pending.delete(data.sequence); this.pendingBytes -= waiter.bytes;
      if (data.sample) this.sample = data.sample;
      if (data.metrics) this.metrics = data.metrics;
      waiter.resolve(data.reply);
      if (this.eventsPending) void this.events();
    };
    worker.onerror = event => this.fail(new Error(event.message));
    worker.onmessageerror = () => this.fail(new Error('Worker message could not be decoded'));
    try {this.stopPthreads = ownPthreads(worker, moduleURL, error => {void this.fail(error);}, compiled, memory);}
    catch (error) {worker.terminate(); throw error;}
  }
  subscribe(handle: Handle, handler: NotificationHandler): () => void {
    const key = handleKey(handle);
    let handlers = this.notificationHandlers.get(key);
    if (!handlers) this.notificationHandlers.set(key, handlers = new Set());
    handlers.add(handler);
    return () => {handlers.delete(handler); if (!handlers.size) this.notificationHandlers.delete(key);};
  }
  unsubscribe(handle: Handle) {
    this.notificationHandlers.get(handleKey(handle))?.clear();
    this.notificationHandlers.delete(handleKey(handle));
  }
  closeEvents() {this.eventsEnabled = false; this.eventsPending = false;}
  private async events() {
    if (this.eventsRunning) return;
    this.eventsRunning = true;
    try {
      while (this.eventsEnabled && this.eventsPending && !this.failure) {
        // A full application queue defers the event drain until a command
        // reply releases capacity; it is not a fatal protocol error.
        if (this.pending.size >= limits.maxPendingCommands || this.pendingBytes + 12 > limits.workerQueueBytes) return;
        this.eventsPending = false;
        const r = await this.call(Op.events), count = r.u32();
        if (count > 16) throw new Error('Invalid event batch');
        const deliveries: {key?: string; value?: Notification; overflow?: NotificationOverflowError}[] = [];
        for (let i = 0; i < count; i++) {
          const kind = r.u32();
          if (kind === 1) {
            const handle = r.handle(), pid = r.i32(), channel = r.text(), payload = r.text();
            if (handle.kind !== 2 || channel === null || payload === null) throw new Error('Invalid notification');
            deliveries.push({key: handleKey(handle), value: {pid, channel, payload}});
          } else if (kind === 2) deliveries.push({overflow: new NotificationOverflowError(r.u64())});
          else throw new Error('Unknown notification event');
        }
        r.end();
        for (const delivery of deliveries) {
          const handlers = delivery.key ? this.notificationHandlers.get(delivery.key) :
            new Set([...this.notificationHandlers.values()].flatMap(values => [...values]));
          for (const handler of [...handlers ?? []]) {
            if (!handlers?.has(handler)) continue;
            if (delivery.overflow) notificationError(handler, delivery.overflow);
            else {
              // Application callbacks run outside reply decoding. A throwing
              // or rejecting callback must not retire the database transport.
              try {Promise.resolve(handler.notify(delivery.value!)).catch(error => notificationError(handler, error));}
              catch (error) {notificationError(handler, error);}
            }
          }
        }
      }
    } catch (error) {if (!this.failure) await this.fail(error as Error);}
    finally {this.eventsRunning = false;}
  }
  private retire(error: Error) {
    if (this.retired) return;
    this.retired = true;
    clearTimeout(this.retirementTimer);
    if (this.failure && this.failure !== error) this.failure.cause = error;
    this.failure ??= error;
    this.readiness.fail(this.failure);
    this.notificationHandlers.clear();
    for (const waiter of this.pending.values()) waiter.reject(this.failure);
    this.pending.clear(); this.pendingBytes = 0;
    const worker = this.worker!, stopPthreads = this.stopPthreads;
    this.worker = undefined; this.stopPthreads = () => {};
    worker.onmessage = null; worker.onerror = null; worker.onmessageerror = null;
    stopPthreads(); worker.terminate();
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
    try {this.worker!.postMessage({discard: String(error)});}
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
      this.pendingBytes += bytes.byteLength; this.pending.set(sequence, {resolve, reject, bytes: bytes.byteLength}); this.worker!.postMessage(bytes, [bytes]);
    });
    try {return response(reply, sequence, op);}
    catch (error) {
      if (!(error instanceof DatabaseError) || error.status === 8) await this.fail(error as Error);
      throw error;
    }
  }
  terminate() {this.retire(new ClosedError());}
  discard(error: Error) {return this.fail(error);}
}
export class Database {
  private transport: Transport;
  private handle: Handle;
  private connections = new Set<Connection>();
  private starting = new Set<Promise<Connection>>();
  private closing = false;
  private nativeClosed = false;
  private closeWork: Promise<unknown> | undefined;
  private archiveCancel: (() => Promise<unknown>) | undefined;
  private archiveWork: Promise<ReadableStream<Uint8Array>> | undefined;
  private constructor(transport: Transport, handle: Handle) {this.transport = transport; this.handle = handle;}
  private static async createTransport(options: OpenOptions): Promise<Transport> {
    const interval = options.diagnostics?.sampleIntervalMs ?? 0;
    if (!Number.isInteger(interval) || interval < 0 || interval > 60000)
      throw new RangeError('sampleIntervalMs must be an integer from 0 to 60000');
    const base = options.assetBase ? new URL(options.assetBase, globalThis.location.href) : new URL('../../', import.meta.url);
    if (!base.pathname.endsWith('/')) throw new Error('assetBase must end with a slash');
    const manifest = await assets(base);
    if (manifest.protocolVersion !== VERSION) throw new Error('postgamma bridge protocol mismatch');
    const [, compiled] = await Promise.all([
      verifiedAsset(base, manifest, manifest.entries.worker), compiledKernel(base, manifest),
    ]);
    const memory = ownedMemory(manifest);
    const worker = new Worker(new URL(manifest.entries.worker, base), {type: 'module'});
    await ownWorkerMailbox(worker);
    return new Transport(worker,
      new URL(manifest.entries.module, base), compiled, memory);
  }
  static async open(options: OpenOptions = {}): Promise<Database> {
    const transport = await Database.createTransport(options);
    try {
      const r = await transport.call(Op.open, undefined, w => openMessage(w, options));
      const handle = r.handle(); r.end(); return new Database(transport, handle);
    } catch (error) {transport.terminate(); throw error;}
  }
  /** Restore a complete archive from this exact asset build into a new target.
   * Failed OPFS targets retain an incomplete marker and cannot be opened. */
  static async loadDataDir(source: ReadableStream<Uint8Array>, options: RestoreOptions = {}): Promise<Database> {
    const decoder = new ArchiveDecoder(source, options.signal);
    let transport: Transport | undefined, prepared = false;
    try {
      const header = await decoder.frame(), expected = archiveHeader();
      if (header.length !== expected.length || !header.every((b, i) => b === expected[i]))
        throw new Error('Database archive format or build identity mismatch');
      options.signal?.throwIfAborted();
      transport = await Database.createTransport(options);
      (await transport.call(Op.restoreOpen, undefined, w => openMessage(w, {...options, create: false}))).end();
      prepared = true;
      for (;;) {
        const record = await decoder.frame(); options.signal?.throwIfAborted();
        if (record.length < 4) throw new Error('Invalid database archive record');
        const last = new DataView(record.buffer, record.byteOffset, record.byteLength).getUint32(0, true) === 0;
        if (last) await decoder.end();
        (await transport.call(Op.archiveWrite, undefined, w => w.raw(record))).end();
        if (last) break;
      }
      options.signal?.throwIfAborted();
      // The Worker owns teardown if native startup rejects a complete image.
      prepared = false;
      const r = await transport.call(Op.restoreFinish, undefined, w => openMessage(w, {...options, create: false}));
      const handle = r.handle(); r.end();
      return new Database(transport, handle);
    } catch (error) {
      if (prepared && transport) {
        try {(await transport.call(Op.stop)).end();} catch { /* Worker fatal cleanup owns failed storage. */ }
      }
      transport?.terminate(); throw error;
    } finally {await decoder.close();}
  }
  /** Close an idle database and stream its complete data directory. Reading
   * to EOF, cancelling the stream or close() releases its storage ownership. */
  dumpDataDir(options: ArchiveOptions = {}): Promise<ReadableStream<Uint8Array>> {
    if (this.archiveWork) return Promise.reject(new Error('Archive is already in progress'));
    const work = this.createArchive(options);
    this.archiveWork = work;
    void work.catch(() => {this.archiveWork = undefined;});
    return work;
  }
  private async createArchive(options: ArchiveOptions): Promise<ReadableStream<Uint8Array>> {
    if (this.closing || this.starting.size) throw new Error('Archive requires an idle database');
    options.signal?.throwIfAborted();
    this.closing = true;
    const frozen: Connection[] = [];
    try {
      for (const connection of this.connections) {connection.freezeForArchive(true); frozen.push(connection);}
      for (const connection of frozen) {
        const status = await connection.status();
        if (status.transaction !== 0 || status.requestId !== 0n) throw new Error('Archive requires no active transactions or requests');
      }
      options.signal?.throwIfAborted();
    } catch (error) {
      for (const connection of frozen) connection.freezeForArchive(false);
      this.closing = false; throw error;
    }
    await this.finishClose(true);
    const resources = (this.transport.metrics as {resources?: unknown})?.resources;
    const encoder = new ArchiveEncoder(); let first = true, finished = false;
    let stopping: Promise<unknown> | undefined;
    const stop = () => stopping ??= (async () => {
      finished = true; options.signal?.removeEventListener('abort', abort);
      try {(await this.transport.call(Op.stop)).end();}
      finally {this.transport.terminate();}
      this.transport.metrics = {...this.transport.metrics as object, resources,
        peakHostQueuedBytes: this.transport.peakHostQueuedBytes};
      return this.transport.metrics;
    })();
    let controller: ReadableStreamDefaultController<Uint8Array>;
    const abort = () => {controller.error(options.signal?.reason); void stop().catch(() => {});};
    const stream = new ReadableStream<Uint8Array>({
      start(value) {controller = value;},
      pull: async value => {
        if (finished) return;
        try {
          options.signal?.throwIfAborted();
          if (first) {first = false; value.enqueue(await encoder.frame(archiveHeader())); return;}
          const r = await this.transport.call(Op.archiveRead), record = r.rest();
          const last = new DataView(record.buffer, record.byteOffset, record.length).getUint32(0, true) === 0;
          const bytes = await encoder.frame(record);
          if (last) await stop();
          if (!options.signal?.aborted) {value.enqueue(bytes); if (last) value.close();}
        } catch (error) {value.error(error); await stop();}
      },
      cancel: async () => {await stop();},
    }, {highWaterMark: 0});
    this.archiveCancel = async () => {controller.error(new ClosedError()); return stop();};
    options.signal?.addEventListener('abort', abort, {once: true});
    if (options.signal?.aborted) abort();
    return stream;
  }
  /** A clone closes the source, then restores its stream into a new target. */
  async clone(options: RestoreOptions = {}): Promise<Database> {
    const archive = await this.dumpDataDir({signal: options.signal});
    try {return await Database.loadDataDir(archive, options);}
    finally {await this.close();}
  }
  async connect(name = 'postgamma-wasm'): Promise<Connection> {
    if (this.closing) throw new ClosedError();
    const timer = setTimeout(() => {
      void this.transport.discard(new Error('Connection startup exceeded its 90s deadline; instance discarded'));
    }, connectionStartupTimeoutMs);
    const work = this.transport.call(Op.connect, this.handle, w => w.text(name)).then(async r => {
      const pending = r.handle(); r.end();
      this.transport.readiness.track(pending);
      try {
        const signal = new AbortController().signal;
        for (;;) {
          const version = this.transport.readiness.version(pending);
          const reply = await this.transport.call(Op.connectPoll, pending), progress = reply.u32();
          if (progress === 2) {
            const h = reply.handle(); reply.end();
            const connection = new Connection(this.transport, h, () => this.connections.delete(connection));
            this.connections.add(connection); return connection;
          }
          reply.end();
          if (progress !== 1) throw new Error('Invalid connection startup progress');
          await this.transport.readiness.wait(pending, version, signal);
        }
      } finally {this.transport.readiness.retire(pending);}
    });
    this.starting.add(work);
    try {return await work;}
    finally {clearTimeout(timer); this.starting.delete(work);}
  }
  diagnostics() {return this.transport.metrics;}
  sample() {return this.transport.sample;}
  async telemetry() {
    const r = await this.transport.call(Op.telemetry, this.handle);
    const t = {workers: r.u32(), connections: r.u64(), requests: r.u64(), running: r.u64(),
      pinned: r.u64(), runnable: r.u64(), queued: r.u64(), rejections: r.u64()}; r.end(); return t;
  }
  close(): Promise<unknown> {
    if (this.archiveCancel) return this.archiveCancel();
    if (this.archiveWork) return this.archiveWork.then(() => this.close(), () => this.finishClose(false));
    return this.finishClose(false);
  }
  private finishClose(preserveStorage: boolean): Promise<unknown> {
    if (this.closeWork) return this.closeWork;
    this.closing = true;
    this.closeWork = (async () => {
      await Promise.allSettled(this.starting);
      await Promise.all([...this.connections].map(connection => connection.close()));
      // An already submitted event drain precedes CLOSE in the Worker queue.
      // Late readiness edges must not submit work after STOP joins the driver.
      this.transport.closeEvents();
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
      if (!preserveStorage || failures.length) {
        try {(await this.transport.call(Op.stop)).end();}
        catch (error) {failures.push(error);}
        finally {this.transport.terminate();}
      }
      this.transport.metrics = {...this.transport.metrics as object, resources,
        peakHostQueuedBytes: this.transport.peakHostQueuedBytes};
      if (failures.length === 1) throw failures[0];
      if (failures.length) throw new AggregateError(failures, 'Database close and teardown failed');
      return this.transport.metrics;
    })().catch(error => {if (!this.nativeClosed) this.closeWork = undefined; throw error;});
    return this.closeWork;
  }
}
async function lifecycleStep(transport: Transport, op: number, handle: Handle): Promise<void> {
  const signal = new AbortController().signal;
  for (;;) {
    const version = transport.readiness.version(handle);
    const reply = await transport.call(op, handle), progress = reply.u32(); reply.end();
    if (progress === 2) return;
    if (progress !== 1) throw new Error('Invalid lifecycle progress');
    await transport.readiness.wait(handle, version, signal);
  }
}

export class Connection {
  private transport: Transport;
  private handle: Handle;
  private requests = new Set<Request>();
  private starting = new Set<Promise<Request>>();
  private closing = false;
  private archiveFrozen = false;
  /** @internal Admission is frozen synchronously before archive status checks. */
  freezeForArchive(frozen: boolean) {
    if (frozen && (this.closing || this.requests.size || this.starting.size || this.queuedQueries))
      throw new Error('Archive requires no active transactions or requests');
    this.archiveFrozen = frozen;
  }
  private closeWork: Promise<void> | undefined;
  private onClosed: () => void;
  constructor(transport: Transport, handle: Handle, onClosed: () => void) {
    this.transport = transport; this.handle = handle; this.onClosed = onClosed;
    this.transport.readiness.track(handle);
  }
  async execute(sql: string, options: ExecuteOptions = {}): Promise<Request> {
    return this.start(Op.execute, w => {
      w.text(sql).u32(options.binary ? 1 : 0).u32(options.chunkRows ?? 0);
      const parameters = options.parameters ?? []; w.u32(parameters.length);
      for (const p of parameters) w.u32(p.typeOid).u32(p.format ?? (p.value instanceof Uint8Array ? 1 : 0))
        .u32(p.value === null ? 1 : 0).blob(p.value === null ? new Uint8Array() :
          typeof p.value === 'string' ? new TextEncoder().encode(p.value) : p.value);
    });
  }
  /** PostgreSQL simple-query script; all statement and transaction boundaries
   * are parsed by the server. Parameters and binary results use execute(). */
  async executeScript(sql: string, options: ScriptOptions = {}): Promise<Request> {
    if (Object.keys(options).some(key => key !== 'chunkRows')) throw new TypeError('Scripts accept only chunkRows');
    return this.start(Op.script, w => w.text(sql).u32(options.chunkRows ?? 0));
  }
  private async start(op: number, write: (writer: Writer) => void): Promise<Request> {
    if (this.closing || this.archiveFrozen) throw new ClosedError();
    const work = this.transport.call(op, this.handle, write).then(r => {
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
    return this.collect(sql, options, false);
  }
  exec(sql: string, options: ScriptOptions = {}): Promise<Result[]> {
    if (Object.keys(options).some(key => key !== 'chunkRows')) return Promise.reject(new TypeError('Scripts accept only chunkRows'));
    return this.collect(sql, options, true);
  }
  /** Subscribe to notifications from SQL LISTEN on this connection. Removing
   * the callback does not execute UNLISTEN or alter transaction state. */
  onNotification(notify: NotificationHandler['notify'], onError?: NotificationHandler['error']): () => void {
    if (this.closing) throw new ClosedError();
    if (typeof notify !== 'function' || (onError !== undefined && typeof onError !== 'function'))
      throw new TypeError('Notification handlers must be functions');
    return this.transport.subscribe(this.handle, {notify, error: onError});
  }
  private collect(sql: string, options: ExecuteOptions, script: boolean): Promise<Result[]> {
    if (this.closing || this.archiveFrozen) return Promise.reject(new ClosedError());
    if (this.queuedQueries >= limits.maxPendingCommands) return Promise.reject(new Error('Connection query queue is full'));
    let bytes = utf8Length(sql) + 64;
    for (const p of options.parameters ?? [])
      bytes += 16 + (typeof p.value === 'string' ? utf8Length(p.value) : p.value?.byteLength ?? 0);
    if (bytes > limits.commandBytes || this.queuedBytes + bytes > limits.connectionQueueBytes)
      return Promise.reject(new Error('Connection query byte queue is full'));
    this.queuedBytes += bytes; this.transport.hostQueuedBytes += bytes;
    this.transport.peakHostQueuedBytes = Math.max(this.transport.peakHostQueuedBytes, this.transport.hostQueuedBytes);
    this.queuedQueries++;
    const work = this.queryTail.then(async () => {
      const request = await (script ? this.executeScript(sql, options) : this.execute(sql, options)), results: Result[] = [];
      let bytes = 0;
      try {for (;;) {
        const batch = await request.next(); if (batch === null) return results;
        if (batch.copy) throw new Error('Use execute() or executeScript() for COPY streams');
        bytes += batch.byteLength;
        if (bytes > limits.collectedResultBytes) throw new Error(`Collected results exceed ${limits.collectedResultBytes} bytes; consume an iterator`);
        results.push(batch);
      }}
      finally {await request.close();}
    });
    // Releasing admission is part of completion. An awaiting caller may
    // immediately submit another large query using the same byte allowance.
    const completed = work.finally(() => {this.queuedQueries--; this.queuedBytes -= bytes; this.transport.hostQueuedBytes -= bytes;});
    this.queryTail = completed.then(() => {}, () => {});
    return completed;
  }
  async status() {
    const r = await this.transport.call(Op.status, this.handle);
    const value = {transaction: r.u32(), pinReasons: r.u32(), carrierRetained: r.u32(), pid: r.i32(),
      connectionId: r.u64(), requestId: r.u64()}; r.end(); return value;
  }
  close(): Promise<void> {
    if (this.closeWork) return this.closeWork;
    this.closing = true;
    this.transport.unsubscribe(this.handle);
    this.closeWork = (async () => {
      await Promise.allSettled(this.starting);
      await Promise.all([...this.requests].map(request => request.close()));
      await this.queryTail;
      const signal = new AbortController().signal;
      for (;;) {
        const version = this.transport.readiness.version(this.handle);
        const reply = await this.transport.call(Op.disconnect, this.handle);
        const availability = reply.u32(); reply.end();
        if (availability === 2) break;
        if (availability !== 1) throw new Error('Invalid connection close progress');
        await this.transport.readiness.wait(this.handle, version, signal);
      }
      this.transport.readiness.retire(this.handle); this.onClosed();
    })().catch(error => {this.closeWork = undefined; throw error;});
    return this.closeWork;
  }
}
export class Request {
  private cancelWork?: Promise<void>;
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
  cancel(): Promise<void> {
    if (this.closing) return Promise.reject(new ClosedError());
    if (!this.cancelWork) this.cancelWork = lifecycleStep(this.transport, Op.cancel, this.handle)
      .finally(() => {this.cancelWork = undefined;});
    return this.cancelWork;
  }
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
      await this.cancelWork?.catch(() => {});
      if (this.copy) await this.copy.close();
      await lifecycleStep(this.transport, Op.release, this.handle);
      this.transport.readiness.retire(this.handle); this.onClosed();
    })().catch(error => {this.closeWork = undefined; throw error;});
    return this.closeWork;
  }
}

export class CopyStream {
  private abortWork?: Promise<void>;
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
  abort(): Promise<void> {
    if (this.closed) return Promise.reject(new ClosedError());
    if (!this.abortWork) this.abortWork = lifecycleStep(this.transport, Op.copyAbort, this.handle)
      .finally(() => {this.abortWork = undefined;});
    return this.abortWork;
  }
  close(): Promise<void> {
    if (this.closeWork) return this.closeWork;
    this.closed = true;
    this.stopped.abort(new ClosedError());
    this.closeWork = (async () => {
      await this.idle;
      await this.abortWork?.catch(() => {});
      await lifecycleStep(this.transport, Op.copyClose, this.handle);
      this.transport.readiness.retire(this.handle);
    })().catch(error => {this.closeWork = undefined; throw error;});
    return this.closeWork;
  }
  async *[Symbol.asyncIterator](): AsyncGenerator<Uint8Array> {
    try {for (;;) {const bytes = await this.read(); if (bytes === null) return; yield bytes;}}
    finally {await this.close();}
  }
}
