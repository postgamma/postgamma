/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
import {DatabaseLockedError} from '../storage/ownership.js';
import type {LockOptions} from '../storage/ownership.js';

/** Only the lightweight control Worker exists while OPFS ownership is pending. */
export function prepareOwnership(worker: Worker, storageName: string | null, options: LockOptions, signal?: AbortSignal): Promise<void> {
  return new Promise((resolve, reject) => {
    const cleanup = () => {
      worker.removeEventListener('message', receive);
      worker.removeEventListener('error', failed);
      worker.removeEventListener('messageerror', unreadable);
      signal?.removeEventListener('abort', abort);
    };
    const finish = (error?: Error) => {cleanup(); if (error) reject(error); else resolve();};
    const failed = (event: ErrorEvent) => {event.preventDefault(); finish(new Error(event.message));};
    const unreadable = () => finish(new Error('Database ownership reply could not be decoded'));
    const abort = () => {cleanup(); reject(signal!.reason);};
    const receive = ({data}: MessageEvent) => {
      if (data?.postgammaPrepared === true) {finish(); return;}
      if (data?.postgammaPrepared === false) {
        const error = data.error;
        // Reconstruct the public class in the caller's realm. Structured clone
        // does not preserve user-defined Error prototypes or custom fields.
        if (error?.code === 'DATABASE_LOCKED' && error.databaseName === storageName &&
            (error.reason === 'busy' || error.reason === 'timeout'))
          finish(new DatabaseLockedError(error.databaseName, error.reason));
        else finish(new Error(error?.message ?? 'Database ownership failed'));
      } else if (data?.fatal) finish(new Error(data.fatal));
    };
    worker.addEventListener('message', receive);
    worker.addEventListener('error', failed);
    worker.addEventListener('messageerror', unreadable);
    signal?.addEventListener('abort', abort, {once: true});
    if (signal?.aborted) {abort(); return;}
    try {worker.postMessage({postgammaPrepare: true, storageName, lock: options.lock, lockTimeoutMs: options.lockTimeoutMs});}
    catch (error) {cleanup(); reject(error);}
  });
}
