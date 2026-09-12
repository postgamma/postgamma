/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
export async function acquire(name: string): Promise<() => Promise<void>> {
  if (!/^[a-zA-Z0-9_-]{1,64}$/.test(name)) throw new Error('Database name must contain 1-64 ASCII letters, digits, underscores or hyphens');
  if (!navigator.locks || !navigator.storage?.getDirectory) throw new Error('OPFS and Web Locks are required');
  let release!: () => void;
  const held = new Promise<void>(resolve => {release = resolve;});
  let ready!: () => void, reject!: (error: unknown) => void;
  const acquired = new Promise<void>((resolve, fail) => {ready = resolve; reject = fail;});
  const work = navigator.locks.request('postgamma:database:' + name,
    {mode: 'exclusive', ifAvailable: true}, async lock => {
      if (!lock) throw new Error('Database already has a live owner');
      ready(); await held;
    });
  work.catch(reject);
  await acquired;
  return async () => {release(); await work;};
}
