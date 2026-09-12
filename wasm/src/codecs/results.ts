/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
import type {CopyStream} from '../api/database.js';
import {Reader} from '../protocol/wire.js';
export interface Column {
  name: string; typeOid: number; format: number; tableOid: number;
  tableColumn: number; typeSize: number; typeModifier: number;
}
export interface Result {byteLength: number; copy?: CopyStream; kind: number; command: string; columns: Column[]; rows: (string | Uint8Array | null)[][]}
export function result(r: Reader): Result {
  const kind = r.u32(), command = r.text() || '', width = r.u32(), height = r.u32();
  if (width > 65535 || height > 1048576) throw new Error('Invalid result dimensions');
  const columns = Array.from({length: width}, () => ({name: r.text() || '', typeOid: r.u32(),
    format: r.u32(), tableOid: r.u32(), tableColumn: r.i32(), typeSize: r.i32(), typeModifier: r.i32()}));
  const decoder = new TextDecoder('utf-8', {fatal: true});
  const rows = Array.from({length: height}, () => columns.map(column => {
    const bytes = r.blob();
    return bytes === null ? null : column.format === 1 ? bytes : decoder.decode(bytes);
  }));
  return {byteLength: r.byteLength, kind, command, columns, rows};
}
