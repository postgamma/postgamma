/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
import {Namespace, checksum, journalLimit, nodeLimit} from './namespace.js';
import type {Node} from './namespace.js';

export const checkpointMagic = 0x324e4750;
export const retiredMagic = 0x524e4750;
export const headerBytes = 32;
export const slotLimit = journalLimit / 2;
const encoder = new TextEncoder(), decoder = new TextDecoder('utf-8', {fatal: true});

export function snapshot(namespace: Namespace, generation: number): Uint8Array {
  if (!Number.isInteger(generation) || generation < 1 || generation > 0xffffffff)
    throw new Error('Namespace checkpoint generation exhausted');
  const nodes = [...namespace.nodes.values()];
  const names = nodes.map(node => encoder.encode(node.name));
  const size = headerBytes + names.reduce((n, name) => n + 20 + name.length, 0);
  // Binary names keep the largest supported namespace below one slot, even
  // when JSON would expand every character into an escape sequence.
  if (size > slotLimit || nodes.length > nodeLimit) throw new Error('Namespace snapshot exceeds its limit');
  const bytes = new Uint8Array(size), view = new DataView(bytes.buffer);
  [checkpointMagic, size, generation, namespace.nextId, nodes.length].forEach((n, i) => view.setUint32(i * 4, n, true));
  let at = headerBytes;
  nodes.forEach((node, i) => {
    const name = names[i];
    [node.id, node.parent, node.kind, node.mode, name.length].forEach((n, j) => view.setUint32(at + j * 4, n, true));
    bytes.set(name, at + 20);
    at += 20 + name.length;
  });
  view.setUint32(20, checksum(bytes), true);
  return bytes;
}

export function restore(bytes: Uint8Array): {namespace: Namespace; generation: number; size: number} {
  if (bytes.length < headerBytes) throw new Error('Incomplete namespace snapshot');
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const size = view.getUint32(4, true), count = view.getUint32(16, true);
  if (view.getUint32(0, true) !== checkpointMagic || size < headerBytes || size > slotLimit ||
      bytes.length < size || !count || count > nodeLimit || !view.getUint32(8, true) ||
      view.getUint32(24, true) || view.getUint32(28, true)) throw new Error('Invalid namespace snapshot header');
  const checked = bytes.slice(0, size);
  new DataView(checked.buffer).setUint32(20, 0, true);
  if (checksum(checked) !== view.getUint32(20, true)) throw new Error('Corrupt namespace snapshot');
  const nodes: Node[] = [];
  let at = headerBytes;
  for (let i = 0; i < count; i++) {
    if (at + 20 > size) throw new Error('Incomplete namespace snapshot node');
    const length = view.getUint32(at + 16, true);
    if (length > 255 || at + 20 + length > size) throw new Error('Invalid namespace snapshot name length');
    nodes.push({id: view.getUint32(at, true), parent: view.getUint32(at + 4, true),
      kind: view.getUint32(at + 8, true), mode: view.getUint32(at + 12, true),
      name: decoder.decode(bytes.subarray(at + 20, at + 20 + length))});
    at += 20 + length;
  }
  if (at !== size) throw new Error('Trailing namespace snapshot bytes');
  return {namespace: Namespace.restore(nodes, view.getUint32(12, true)), generation: view.getUint32(8, true), size};
}
