/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
import {limits} from './limits.js';
export const VERSION = 3;
export const LIMIT = limits.commandBytes;
export const Op = {open: 1, connect: 2, execute: 3, next: 4, release: 5, disconnect: 6,
  close: 7, status: 8, cancel: 9, telemetry: 10, stop: 11, copyRead: 12, copyWrite: 13, copyFinish: 14, copyClose: 15, copyAbort: 16} as const;
export interface Handle {kind: number; id: number; generation: number}
export class Writer {
  private parts: Uint8Array[] = [];
  private size = 0;
  u32(value: number): this {
    if (!Number.isInteger(value) || value < 0 || value > 0xffffffff) throw new RangeError('Expected uint32');
    const bytes = new Uint8Array(4); new DataView(bytes.buffer).setUint32(0, value, true);
    return this.raw(bytes);
  }
  raw(bytes: Uint8Array): this {
    if (this.size + bytes.length > LIMIT) throw new RangeError('Wire message exceeds limit');
    this.parts.push(bytes); this.size += bytes.length; return this;
  }
  blob(bytes: Uint8Array): this {return this.u32(bytes.length).raw(bytes);}
  text(value: string): this {return this.blob(new TextEncoder().encode(value));}
  handle(value: Handle): this {return this.u32(value.id).u32(value.generation);}
  finish(): ArrayBuffer {
    const result = new Uint8Array(this.size); let offset = 0;
    for (const part of this.parts) {result.set(part, offset); offset += part.length;}
    return result.buffer;
  }
}
export class Reader {
  private offset = 0;
  private view: DataView;
  private buffer: ArrayBuffer;
  constructor(buffer: ArrayBuffer) {
    this.buffer = buffer;
    if (!(buffer instanceof ArrayBuffer) || buffer.byteLength > LIMIT) throw new Error('Invalid wire buffer');
    this.view = new DataView(buffer);
  }
  u32(): number {
    if (this.offset + 4 > this.buffer.byteLength) throw new Error('Truncated wire integer');
    const result = this.view.getUint32(this.offset, true); this.offset += 4; return result;
  }
  i32(): number {return this.u32() | 0;}
  u64(): bigint {const low = this.u32(); return BigInt(low) | BigInt(this.u32()) << 32n;}
  blob(): Uint8Array | null {
    const length = this.u32(); if (length === 0xffffffff) return null;
    if (this.offset + length > this.buffer.byteLength) throw new Error('Truncated wire value');
    const result = new Uint8Array(this.buffer, this.offset, length); this.offset += length; return result;
  }
  text(): string | null {const value = this.blob(); return value === null ? null : new TextDecoder('utf-8', {fatal: true}).decode(value);}
  handle(): Handle {return {kind: this.u32(), id: this.u32(), generation: this.u32()};}
  get byteLength(): number {return this.buffer.byteLength;}
  end(): void {if (this.offset !== this.buffer.byteLength) throw new Error('Trailing wire data');}
}
export class DatabaseError extends Error {
  readonly sqlstate: string | null;
  readonly status: number;
  readonly diagnostics: (string | null)[];
  constructor(status: number, diagnostics: (string | null)[]) {
    super(diagnostics[2] || `Database operation failed (${status})`);
    this.status = status; this.diagnostics = diagnostics;
    this.name = 'DatabaseError'; this.sqlstate = diagnostics[0];
  }
}
export function response(buffer: ArrayBuffer, sequence: number, op: number): Reader {
  const r = new Reader(buffer);
  if (r.u32() !== VERSION || r.u32() !== sequence || r.u32() !== op) throw new Error('Stale or incompatible response');
  const status = r.u32(), diagnostics = Array.from({length: 17}, () => r.text());
  if (status) {r.end(); throw new DatabaseError(status, diagnostics);}
  return r;
}
