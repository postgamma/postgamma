/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
export interface LockOptions {
  /** Wait for a previous owner by default, or reject immediately. */
  lock?: 'wait' | 'fail';
  /** Lock acquisition deadline in active milliseconds; default 5000. */
  lockTimeoutMs?: number;
}
export class DatabaseLockedError extends Error {
  readonly code = 'DATABASE_LOCKED';
  readonly databaseName: string;
  readonly reason: 'busy' | 'timeout';
  constructor(databaseName: string, reason: 'busy' | 'timeout') {
    super((reason === 'timeout' ? 'Timed out waiting: ' : '') + `Database '${databaseName}' already has a live owner`);
    this.name = 'DatabaseLockedError'; this.databaseName = databaseName; this.reason = reason;
  }
}
export function validateLockOptions(options: LockOptions): void {
  if (options.lock !== undefined && options.lock !== 'wait' && options.lock !== 'fail')
    throw new TypeError('lock must be wait or fail');
  if (options.lockTimeoutMs !== undefined && (!Number.isInteger(options.lockTimeoutMs) ||
      options.lockTimeoutMs < 0 || options.lockTimeoutMs > 2147483647))
    throw new RangeError('lockTimeoutMs must be an integer from 0 to 2147483647');
}
export async function acquire(name: string, options: LockOptions = {}, signal?: AbortSignal): Promise<() => Promise<void>> {
  validateLockOptions(options);
  signal?.throwIfAborted();
  if (!/^[a-zA-Z0-9_-]{1,64}$/.test(name)) throw new Error('Database name must contain 1-64 ASCII letters, digits, underscores or hyphens');
  if (!navigator.locks || !navigator.storage?.getDirectory || typeof BroadcastChannel !== 'function')
    throw new Error('OPFS, Web Locks and BroadcastChannel are required');
  let release!: () => void;
  const held = new Promise<void>(resolve => {release = resolve;});
  let ready!: () => void, reject!: (error: unknown) => void;
  const acquired = new Promise<void>((resolve, fail) => {ready = resolve; reject = fail;});
  const controller = new AbortController();
  const abort = () => controller.abort(signal!.reason);
  signal?.addEventListener('abort', abort, {once: true});
  const timeoutMs = options.lockTimeoutMs ?? 5000;
  // A zero deadline still makes one immediate attempt at a free lock.
  const immediate = options.lock === 'fail' || timeoutMs === 0;
  const timer = immediate ? undefined : setTimeout(() => controller.abort(new DatabaseLockedError(name, 'timeout')), timeoutMs);
  try {
    const work = navigator.locks.request('postgamma:database:' + name,
      immediate ? {mode: 'exclusive', ifAvailable: true} : {mode: 'exclusive', signal: controller.signal}, async lock => {
        controller.signal.throwIfAborted();
        if (!lock) throw new DatabaseLockedError(name, options.lock === 'fail' ? 'busy' : 'timeout');
        ready(); await held;
      });
    work.catch(reject);
    await acquired;
    return async () => {release(); await work;};
  } finally {
    clearTimeout(timer); signal?.removeEventListener('abort', abort);
  }
}
