/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
export interface Node {id: number; parent: number; name: string; kind: number; mode: number}
export type Change = {op: 'create'; node: Node} | {op: 'move'; id: number; parent: number; name: string} |
  {op: 'remove'; parent: number; name: string};
const magic = 0x314e4750;
const encoder = new TextEncoder(), decoder = new TextDecoder('utf-8', {fatal: true});
export const journalLimit = 16 * 1024 * 1024;
export const nodeLimit = 16384;
export function checksum(bytes: Uint8Array): number {
  let crc = 0xffffffff;
  for (const byte of bytes) {
    crc ^= byte;
    for (let i = 0; i < 8; i++) crc = crc >>> 1 ^ (crc & 1 ? 0xedb88320 : 0);
  }
  return (crc ^ 0xffffffff) >>> 0;
}
export function encode(change: Change, sequence: number): Uint8Array {
  const payload = encoder.encode(JSON.stringify(change));
  if (payload.length > 4096 || !Number.isSafeInteger(sequence) || sequence < 1 || sequence >= 0x80000000)
    throw new Error('Namespace record exceeds the format');
  const bytes = new Uint8Array(16 + payload.length), view = new DataView(bytes.buffer);
  view.setUint32(0, magic, true); view.setUint32(4, payload.length, true);
  view.setUint32(8, sequence, true); bytes.set(payload, 16);
  // Include sequence and length in the checksum, as well as the payload.
  view.setUint32(12, checksum(bytes), true);
  return bytes;
}
export class Namespace {
  nodes = new Map<number, Node>([[1, {id: 1, parent: 0, name: '', kind: 2, mode: 0o700}]]);
  sequence = 0;
  nextId = 2;
  lookup(parent: number, name: string): Node | undefined {
    for (const node of this.nodes.values()) if (node.parent === parent && node.name === name) return node;
  }
  children(parent: number): Node[] {return [...this.nodes.values()].filter(n => n.parent === parent);}
  apply(change: Change) {
    const validName = (name: string) => typeof name === 'string' && name.length > 0 &&
      name !== '.' && name !== '..' && !/[\0/]/.test(name) && encoder.encode(name).length <= 255;
    const parent = change.op === 'create' ? change.node.parent : change.parent;
    const name = change.op === 'create' ? change.node.name : change.name;
    if (this.nodes.get(parent)?.kind !== 2 || !validName(name)) throw new Error('Invalid namespace parent/name');
    const replaced = this.lookup(parent, name);
    if (change.op === 'create') {
      const n = change.node;
      if (!Number.isSafeInteger(n.id) || n.id < this.nextId || n.id >= 0x80000000 ||
          ![1, 2].includes(n.kind) || !Number.isInteger(n.mode) || n.mode < 0 || n.mode > 0o7777 ||
          replaced || this.nodes.size >= nodeLimit) throw new Error('Invalid namespace creation');
      this.nodes.set(n.id, {...n}); this.nextId = n.id + 1;
    } else if (change.op === 'move') {
      const node = this.nodes.get(change.id);
      if (!node || node.id === 1 || node === replaced) throw new Error('Invalid namespace move');
      for (let p = this.nodes.get(parent); p; p = this.nodes.get(p.parent))
        if (p.id === node.id) throw new Error('Namespace cycle');
      if (replaced && (replaced.kind !== node.kind || replaced.kind === 2 && this.children(replaced.id).length))
        throw new Error('Invalid namespace replacement');
      if (replaced) this.nodes.delete(replaced.id);
      node.parent = parent; node.name = name;
    } else if (change.op === 'remove') {
      if (!replaced || replaced.kind === 2 && this.children(replaced.id).length) throw new Error('Invalid namespace removal');
      this.nodes.delete(replaced.id);
    } else throw new Error('Unknown namespace operation');
  }
  static replay(bytes: Uint8Array): {namespace: Namespace; validBytes: number; discardedBytes: number} {
    if (bytes.length > journalLimit) throw new Error('Namespace journal exceeds its limit');
    const namespace = new Namespace();
    let offset = 0;
    while (offset < bytes.length) {
      if (bytes.length - offset < 16) break;
      const view = new DataView(bytes.buffer, bytes.byteOffset + offset, bytes.length - offset);
      const length = view.getUint32(4, true);
      if (view.getUint32(0, true) !== magic || length > 4096) throw new Error('Corrupt namespace header');
      if (16 + length > bytes.length - offset) break;
      const record = Uint8Array.from(bytes.subarray(offset, offset + 16 + length)), copy = new DataView(record.buffer);
      const crc = copy.getUint32(12, true); copy.setUint32(12, 0, true);
      if (checksum(record) !== crc) {
        if (offset + record.length !== bytes.length) throw new Error('Corrupt interior namespace record');
        break;
      }
      if (view.getUint32(8, true) !== namespace.sequence + 1) throw new Error('Namespace sequence mismatch');
      namespace.apply(JSON.parse(decoder.decode(record.subarray(16))));
      namespace.sequence++; offset += record.length;
    }
    return {namespace, validBytes: offset, discardedBytes: bytes.length - offset};
  }
}
