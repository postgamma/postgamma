/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
import type {Handle} from './wire.js';

const key = (h: Handle) => `${h.kind}:${h.id}:${h.generation}`;
interface Entry {version: number; waiters: Set<() => void>}

/** Data consumption and lifecycle control can wait independently on one handle. */
export class Readiness {
  private entries = new Map<string, Entry>();
  private failure?: Error;

  track(handle: Handle) {
    const name = key(handle);
    if (this.entries.has(name)) throw new Error('Duplicate live readiness handle');
    this.entries.set(name, {version: 0, waiters: new Set()});
  }
  version(handle: Handle): number {
    if (this.failure) throw this.failure;
    const entry = this.entries.get(key(handle));
    if (!entry) throw new Error('Retired readiness handle');
    return entry.version;
  }
  notify(handle: Handle) {
    const entry = this.entries.get(key(handle));
    if (!entry) return; // A queued event cannot revive a retired generation.
    entry.version++;
    for (const wake of entry.waiters) wake();
  }
  async wait(handle: Handle, version: number, signal: AbortSignal): Promise<void> {
    if (signal.aborted) throw signal.reason;
    if (this.version(handle) !== version) return;
    const entry = this.entries.get(key(handle))!;
    let wake!: () => void;
    try {
      await new Promise<void>(resolve => {
        wake = resolve; entry.waiters.add(wake);
        signal.addEventListener('abort', wake, {once: true});
      });
    } finally {
      signal.removeEventListener('abort', wake);
      entry.waiters.delete(wake);
    }
    if (signal.aborted) throw signal.reason;
    this.version(handle); // Retirement or transport failure also wakes waiters.
  }
  retire(handle: Handle) {
    const name = key(handle);
    for (const wake of this.entries.get(name)?.waiters ?? []) wake();
    this.entries.delete(name);
  }
  fail(error: Error) {
    this.failure = error;
    for (const entry of this.entries.values()) for (const wake of entry.waiters) wake();
    this.entries.clear();
  }
}
