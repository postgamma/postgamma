/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
import {Namespace, encode, journalLimit, legacyMagic} from './namespace.js';
import type {Change} from './namespace.js';
import {headerBytes, retiredMagic, restore, slotLimit, snapshot} from './checkpoint.js';
import type {AccessHandle, StorageDirectory} from './files.js';

export const journalNames = ['namespace-v1', 'namespace-v2'] as const;
export interface JournalHooks {
  point(operation: string, path: string): string | Promise<string>;
  partial(bytes: number): Promise<never>;
  partialCheckpoint?(): boolean;
}
const noHooks: JournalHooks = {
  point: () => '',
  partial: async () => {throw new Error('Interrupted namespace tail requires recovery');},
};

export class Journal {
  namespace = new Namespace();
  private handles: AccessHandle[] = [];
  private sizes = [0, 0];
  private active = 0;
  private generation = 0;
  private hooks: JournalHooks;
  readonly metrics = {compactions: 0, journalBytes: 0, peakJournalBytes: 0,
    discardedJournalBytes: 0, journalGeneration: 0};

  private constructor(hooks: JournalHooks) {this.hooks = hooks;}

  static async open(root: StorageDirectory, hooks = noHooks): Promise<Journal> {
    const journal = new Journal(hooks);
    try {
      let primary;
      let fresh = false;
      try {primary = await root.getFileHandle(journalNames[0]);}
      catch (cause) {
        if ((cause as DOMException).name !== 'NotFoundError') throw cause;
        for await (const _entry of root.entries())
          throw new DOMException('Namespace journal is missing from nonempty database storage', 'NotFoundError');
        primary = await root.getFileHandle(journalNames[0], {create: true});
        fresh = true;
      }
      journal.handles.push(await primary.createSyncAccessHandle());
      const first = journal.read(0);
      const magic = first.length >= 4 ? new DataView(first.buffer).getUint32(0, true) : 0;
      const legacy = magic === legacyMagic;
      const secondary = await root.getFileHandle(journalNames[1], {create: fresh || legacy || first.length === 0});
      journal.handles.push(await secondary.createSyncAccessHandle());
      const second = journal.read(1);
      const candidates: {slot: number; state: ReturnType<typeof restore>}[] = [];
      for (const [slot, bytes] of [first, second].entries()) {
        try {candidates.push({slot, state: restore(bytes)});}
        catch { /* A partially written inactive snapshot has no authority. */ }
      }
      candidates.sort((a, b) => b.state.generation - a.state.generation);
      journal.generation = candidates[0]?.state.generation ?? 0;
      if (legacy) {
        // An interrupted migration can leave a valid legacy log. Prefer it:
        // an older client may have appended to it before the next mount.
        const replay = Namespace.replay(first);
        journal.namespace = replay.namespace;
        journal.metrics.discardedJournalBytes = replay.discardedBytes;
        await journal.checkpoint();
      } else if (candidates.length) {
        if (candidates.length === 2 && candidates[0].state.generation === candidates[1].state.generation)
          throw new Error('Ambiguous namespace checkpoint generations');
        const {slot, state} = candidates[0];
        journal.active = slot;
        // Once a complete snapshot is selected, a corrupt log is an error.
        // Falling back to an older snapshot could discard acknowledged work.
        const replay = Namespace.replay([first, second][slot].subarray(state.size), state.namespace);
        journal.namespace = replay.namespace;
        journal.metrics.discardedJournalBytes = replay.discardedBytes;
        if (replay.discardedBytes) journal.truncate(slot, state.size + replay.validBytes);
        journal.flush();
        await journal.retire(1 - slot);
      } else if (fresh || first.length === 0) {
        for await (const [name] of root.entries()) {
          if (!(journalNames as readonly string[]).includes(name))
            throw new Error('Empty namespace journal in nonempty database storage');
        }
        await journal.checkpoint();
      } else {
        throw new Error('No complete namespace checkpoint is available');
      }
      journal.observe();
      return journal;
    } catch (cause) {
      try {journal.close();} catch { /* Preserve the opening error. */ }
      throw cause;
    }
  }

  private read(slot: number): Uint8Array {
    const handle = this.handles[slot], size = handle.getSize();
    if (!Number.isSafeInteger(size) || size < 0 || size > journalLimit)
      throw new Error('Namespace journal exceeds its limit');
    this.sizes[slot] = size;
    const bytes = new Uint8Array(size);
    let at = 0;
    while (at < size) {
      const n = handle.read(bytes.subarray(at), {at});
      if (!Number.isSafeInteger(n) || n <= 0 || n > size - at) throw new Error('Short namespace read');
      at += n;
    }
    return bytes;
  }

  private observe() {
    this.metrics.journalBytes = this.sizes[0] + this.sizes[1];
    this.metrics.peakJournalBytes = Math.max(this.metrics.peakJournalBytes, this.metrics.journalBytes);
    this.metrics.journalGeneration = this.generation;
  }

  private write(slot: number, bytes: Uint8Array, at: number) {
    let offset = 0;
    while (offset < bytes.length) {
      const n = this.handles[slot].write(bytes.subarray(offset), {at: at + offset});
      if (!Number.isSafeInteger(n) || n <= 0 || n > bytes.length - offset)
        throw new Error('Namespace write made invalid progress');
      offset += n;
      this.sizes[slot] = Math.max(this.sizes[slot], at + offset);
      this.observe();
    }
  }

  private truncate(slot: number, size: number) {
    this.handles[slot].truncate(size);
    this.sizes[slot] = size;
    this.observe();
  }

  private async retire(slot: number, point?: string) {
    const marker = new Uint8Array(headerBytes);
    new DataView(marker.buffer).setUint32(0, retiredMagic, true);
    // Never leave namespace-v1 empty: a legacy reader must reject an upgraded
    // database rather than interpret its data files as orphans of an empty log.
    this.write(slot, marker, 0);
    this.handles[slot].flush();
    const gate = point && this.hooks.point(point, 'snapshot');
    if (gate) await gate;
    this.truncate(slot, marker.length);
    this.handles[slot].flush();
  }

  async checkpoint(): Promise<void> {
    const target = 1 - this.active;
    const bytes = snapshot(this.namespace, this.generation + 1);
    let gate = this.hooks.point('checkpoint-before', 'snapshot');
    if (gate) await gate;
    // The inactive slot is never needed for recovery of acknowledged writes.
    // Invalidate its old checksum before writing another generation.
    await this.retire(target, 'checkpoint-prepared');
    if (this.hooks.partialCheckpoint?.()) {
      const middle = Math.floor(bytes.length / 2);
      this.write(target, bytes.subarray(0, middle), 0);
      this.handles[target].flush();
      gate = this.hooks.point('checkpoint-partial', 'snapshot');
      if (gate) await gate;
      this.write(target, bytes.subarray(middle), middle);
    } else this.write(target, bytes, 0);
    gate = this.hooks.point('checkpoint-written', 'snapshot');
    if (gate) await gate;
    this.handles[target].flush();
    gate = this.hooks.point('checkpoint-flushed', 'snapshot');
    if (gate) await gate;
    // No new mutation may be acknowledged until the old slot is retired.
    // Before this point both complete generations describe the same state.
    await this.retire(this.active, 'checkpoint-retired');
    this.active = target;
    this.generation++;
    this.namespace.sequence = 0;
    this.metrics.compactions++;
    this.observe();
    gate = this.hooks.point('checkpoint-published', 'snapshot');
    if (gate) await gate;
  }

  async append(change: Change): Promise<void> {
    let record = encode(change, this.namespace.sequence + 1);
    if (this.sizes[this.active] + record.length > slotLimit) {
      await this.checkpoint();
      record = encode(change, this.namespace.sequence + 1);
    }
    if (this.sizes[this.active] + record.length > slotLimit)
      throw new DOMException('Namespace snapshot leaves no journal space', 'QuotaExceededError');
    const at = this.sizes[this.active];
    let gate = this.hooks.point('namespace-before', change.op);
    const action = gate ? await gate : '';
    if (action === 'partial') {
      const partial = record.subarray(0, Math.floor(record.length / 2));
      this.write(this.active, partial, at);
      this.flush();
      await this.hooks.partial(partial.length);
    }
    this.write(this.active, record, at);
    gate = this.hooks.point('namespace-written', change.op);
    if (gate) await gate;
    this.flush();
    gate = this.hooks.point('namespace-flushed', change.op);
    if (gate) await gate;
    this.namespace.apply(change);
    this.namespace.sequence++;
  }

  flush() {this.handles[this.active].flush();}

  close() {
    let failure: unknown;
    const remaining: AccessHandle[] = [];
    for (const handle of this.handles) {
      try {handle.close();} catch (cause) {failure ??= cause; remaining.push(handle);}
    }
    this.handles = remaining;
    if (failure) throw failure;
  }
}
