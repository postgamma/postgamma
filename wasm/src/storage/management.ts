/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
import {acquire} from './ownership.js';
import type {LockOptions} from './ownership.js';

export interface DeleteOptions extends LockOptions {signal?: AbortSignal}
const missing = (error: unknown) => error instanceof DOMException && error.name === 'NotFoundError';
type Directory = FileSystemDirectoryHandle & {entries(): AsyncIterableIterator<[string, FileSystemHandle]>};
async function directory(): Promise<Directory | undefined> {
  const root = await navigator.storage.getDirectory();
  try {return await root.getDirectoryHandle('postgamma') as Directory;}
  catch (error) {if (!missing(error)) throw error;}
}
/** A name snapshot, including incomplete restores; enumeration never opens a
 * database or creates directories. SQL database names inside a cluster differ. */
export async function listDatabases(): Promise<string[]> {
  const root = await directory(), names: string[] = [];
  if (root) for await (const [name, entry] of root.entries()) {
    if (entry.kind === 'directory' && /^[a-zA-Z0-9_-]{1,64}$/.test(name)) names.push(name);
  }
  return names.sort();
}
/** Hold the same lock as OPEN/restore until recursive deletion completes. An
 * abort after removeEntry starts cannot cancel or roll back that operation. */
export async function deleteDatabase(name: string, options: DeleteOptions = {}): Promise<boolean> {
  const release = await acquire(name, options, options.signal);
  try {
    options.signal?.throwIfAborted();
    const root = await directory();
    options.signal?.throwIfAborted();
    if (!root) return false;
    try {await root.removeEntry(name, {recursive: true}); return true;}
    catch (error) {if (missing(error)) return false; throw error;}
  } finally {await release();}
}
