/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
import assert from 'node:assert/strict';
import {pathToFileURL} from 'node:url';
const {Reader, Writer, response, VERSION} = await import(pathToFileURL(process.argv[2]));
const packet = new Writer().u32(VERSION).u32(7).u32(4).u32(0);
for (let i = 0; i < 17; i++) packet.u32(0xffffffff);
const wire = packet.finish();
response(wire, 7, 4).end();
assert.throws(() => response(wire, 8, 4), /Stale/);
assert.throws(() => response(wire, 7, 3), /Stale/);
for (let size = 0; size < wire.byteLength; size++)
  assert.throws(() => response(wire.slice(0, size), 7, 4), /Truncated/);
assert.throws(() => new Writer().u32(2 ** 32), /uint32/);
assert.throws(() => new Writer().blob(new Uint8Array(2 * 1024 * 1024)), /limit/);
assert.equal(new Reader(new Writer().u32(0xffffffff).u32(0x7fffffff).finish()).u64(), 9223372036854775807n);
console.log('wire: sequence, operation, truncation, bounds and uint64 passed');
