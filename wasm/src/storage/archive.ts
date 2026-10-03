/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
/* Bounded, chained SHA-256 frames. The final zero record is mandatory: a
 * checksum on each file alone would not detect removal/reordering of files. */
export const archiveRecordLimit = 256 * 1024 + 8192;
export type ArchiveSource = ReadableStream<Uint8Array>;
const digest = async (previous: Uint8Array, payload: Uint8Array) => {
  const input = new Uint8Array(previous.length + payload.length);
  input.set(previous); input.set(payload, previous.length);
  return new Uint8Array(await crypto.subtle.digest('SHA-256', input));
};
export class ArchiveEncoder {
  private previous: Uint8Array = new Uint8Array(32);
  async frame(payload: Uint8Array): Promise<Uint8Array> {
    if (!payload.length || payload.length > archiveRecordLimit) throw new Error('Invalid archive record size');
    this.previous = await digest(this.previous, payload);
    const bytes = new Uint8Array(36 + payload.length);
    new DataView(bytes.buffer).setUint32(0, payload.length, true);
    bytes.set(this.previous, 4); bytes.set(payload, 36); return bytes;
  }
}
export class ArchiveDecoder {
  private reader: ReadableStreamDefaultReader<Uint8Array>;
  private previous: Uint8Array = new Uint8Array(32);
  private chunk: Uint8Array = new Uint8Array();
  private offset = 0;
  private ended = false;
  private aborted: (() => void) | undefined;
  private signal?: AbortSignal;
  constructor(source: ArchiveSource, signal?: AbortSignal) {
    this.signal = signal;
    this.reader = source.getReader();
    if (signal) {
      this.aborted = () => {void this.reader.cancel(signal.reason).catch(() => {});};
      signal.addEventListener('abort', this.aborted, {once: true});
      if (signal.aborted) this.aborted();
    }
  }
  private async read(size: number, eof = false): Promise<Uint8Array | null> {
    const bytes = new Uint8Array(size); let at = 0;
    while (at < size) {
      this.signal?.throwIfAborted();
      if (this.offset === this.chunk.length) {
        const next = await this.reader.read();
        this.signal?.throwIfAborted();
        if (next.done) {this.ended = true; if (!at && eof) return null; throw new Error('Truncated database archive');}
        if (!(next.value instanceof Uint8Array)) throw new TypeError('Archive source must yield Uint8Array');
        this.chunk = next.value; this.offset = 0;
        if (!this.chunk.length) continue;
      }
      const n = Math.min(size - at, this.chunk.length - this.offset);
      bytes.set(this.chunk.subarray(this.offset, this.offset + n), at); at += n; this.offset += n;
    }
    return bytes;
  }
  async frame(): Promise<Uint8Array> {
    const header = (await this.read(36))!;
    const size = new DataView(header.buffer).getUint32(0, true);
    if (!size || size > archiveRecordLimit) throw new Error('Invalid archive record size');
    const payload = (await this.read(size))!;
    this.previous = await digest(this.previous, payload);
    if (!this.previous.every((byte, i) => byte === header[i + 4])) throw new Error('Database archive checksum mismatch');
    return payload;
  }
  async end() {if (await this.read(1, true)) throw new Error('Trailing database archive data');}
  async close(reason?: unknown) {
    if (this.aborted) this.signal?.removeEventListener('abort', this.aborted);
    try {if (!this.ended) await this.reader.cancel(reason);} finally {this.reader.releaseLock();}
  }
}
