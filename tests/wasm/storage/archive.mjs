/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
import assert from 'node:assert/strict';
import {pathToFileURL} from 'node:url';
const {ArchiveEncoder, ArchiveDecoder, archiveRecordLimit} = await import(pathToFileURL(process.argv[2]));
const payloads = [new TextEncoder().encode('header'), new Uint8Array(256 * 1024).fill(71), new Uint8Array(4)];
const encoder = new ArchiveEncoder(), frames = [];
for (const payload of payloads) frames.push(await encoder.frame(payload));
const bytes = Buffer.concat(frames);
function source(data, stride = data.length) {
  let at = 0;
  return new ReadableStream({pull(controller) {
    if (at === data.length) {controller.close(); return;}
    controller.enqueue(new Uint8Array(data.subarray(at, at + stride))); at = Math.min(data.length, at + stride);
  }});
}
for (const stride of [1, 17, 65536, bytes.length]) {
  const reader = new ArchiveDecoder(source(bytes, stride));
  for (const payload of payloads) assert.deepEqual(await reader.frame(), payload);
  await reader.end(); await reader.close();
}
for (const invalid of [Buffer.concat([frames[1], frames[0], frames[2]]),
  Buffer.concat([frames[0], frames[2]]), Buffer.from(bytes)]) {
  if (invalid.length === bytes.length && invalid.equals(bytes)) invalid[invalid.length - 1] ^= 1;
  const reader = new ArchiveDecoder(source(invalid));
  await assert.rejects(async () => {for (let i = 0; i < 3; i++) await reader.frame();}, /checksum/);
  await reader.close();
}
for (const cut of [0, 3, 35, 36, 100, bytes.length - 1]) {
  const reader = new ArchiveDecoder(source(bytes.subarray(0, cut)));
  await assert.rejects(async () => {for (let i = 0; i < 3; i++) await reader.frame();}, /Truncated/);
  await reader.close();
}
const oversize = Buffer.alloc(36); oversize.writeUInt32LE(archiveRecordLimit + 1);
const oversized = new ArchiveDecoder(source(oversize));
await assert.rejects(oversized.frame(), /record size/); await oversized.close();
const signal = new AbortController(); let cancelled = false;
const waiting = new ArchiveDecoder(new ReadableStream({cancel() {cancelled = true;}}), signal.signal);
const pending = waiting.frame(); signal.abort(new Error('cancelled read'));
await assert.rejects(pending, /cancelled read/); await waiting.close(); assert(cancelled);
console.log('Archive framing: arbitrary boundaries, corruption, order, omission, truncation, limits and cancellation passed');
