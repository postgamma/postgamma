/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
import {Namespace, encode} from '../src/storage/namespace.js';
import type {Change} from '../src/storage/namespace.js';
import {restore} from '../src/storage/checkpoint.js';
import {journalNames} from '../src/storage/journal.js';
import {assert} from '../tests/helpers.js';

// Re-encode a closed test database's exact namespace in the previous format.
// Physical PostgreSQL files are unchanged, so reopening tests real migration.
export async function legacyFixture(root: FileSystemDirectoryHandle) {
  const candidates = [];
  for (const name of journalNames) {
    const bytes = new Uint8Array(await (await (await root.getFileHandle(name)).getFile()).arrayBuffer());
    try {
      const state = restore(bytes);
      candidates.push({generation: state.generation,
        namespace: Namespace.replay(bytes.subarray(state.size), state.namespace).namespace});
    } catch { /* Retired slots have no snapshot. */ }
  }
  candidates.sort((a, b) => b.generation - a.generation);
  assert(candidates.length > 0, 'A complete namespace is available for the legacy fixture');
  const namespace = candidates[0].namespace;
  const nodes = [...namespace.nodes.values()].filter(node => node.id !== 1).sort((a, b) => a.id - b.id);
  const changes: Change[] = [];
  const prefix = 'fixture-' + crypto.randomUUID() + '-';
  // Create IDs in order, then reconstruct parent/name relationships. This
  // also represents old children moved under parents created more recently.
  for (const node of nodes) changes.push({op: 'create', node: {...node, parent: 1, name: prefix + node.id}});
  if (namespace.nextId > (nodes.at(-1)?.id ?? 1) + 1) {
    changes.push({op: 'create', node: {id: namespace.nextId - 1, parent: 1, name: prefix + 'deleted', kind: 2, mode: 448}});
    changes.push({op: 'remove', parent: 1, name: prefix + 'deleted'});
  }
  for (const node of nodes) changes.push({op: 'move', id: node.id, parent: node.parent, name: node.name});
  const records = changes.map((change, i) => encode(change, i + 1));
  const bytes = new Uint8Array(records.reduce((size, record) => size + record.length, 0));
  let at = 0;
  for (const record of records) {bytes.set(record, at); at += record.length;}
  const replay = Namespace.replay(bytes).namespace;
  assert(replay.nextId === namespace.nextId && replay.nodes.size === namespace.nodes.size, 'Legacy identity watermark');
  for (const node of nodes) assert(JSON.stringify(replay.nodes.get(node.id)) === JSON.stringify(node), 'Legacy file identity');
  const writer = await (await root.getFileHandle(journalNames[0])).createWritable();
  await writer.write(bytes); await writer.close();
  await root.removeEntry(journalNames[1]);
}
