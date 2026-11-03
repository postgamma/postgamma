/*
 * Copyright 2026 Shujie Zhang
 * SPDX-License-Identifier: Apache-2.0
 */

// Generated from manifests/wasm-storage-protocol.json; do not edit.
export const StorageOp = {
  "mount": 0,
  "lookup": 1,
  "createFile": 2,
  "createDirectory": 3,
  "move": 4,
  "remove": 5,
  "list": 6,
  "open": 7,
  "close": 8,
  "read": 9,
  "write": 10,
  "flush": 11,
  "shutdown": 12,
  "flushDirectory": 13,
  "size": 14,
  "truncate": 15
} as const;
export type StorageOperation = (typeof StorageOp)[keyof typeof StorageOp];
export const StorageErrno = {
  "EACCES": 2,
  "EBUSY": 10,
  "EINVAL": 28,
  "EIO": 29,
  "ENOENT": 44,
  "ENOSPC": 51,
  "ENOTSUP": 138
} as const;
const domErrors: Readonly<Record<string, number>> = {
  QuotaExceededError: StorageErrno.ENOSPC,
  NotFoundError: StorageErrno.ENOENT,
  NoModificationAllowedError: StorageErrno.EBUSY,
  InvalidStateError: StorageErrno.EBUSY,
  NotAllowedError: StorageErrno.EACCES,
  TypeError: StorageErrno.EINVAL,
  NotSupportedError: StorageErrno.ENOTSUP,
};
export function storageErrorCode(cause: unknown): number {
  if (cause && typeof cause === 'object' && 'name' in cause && typeof cause.name === 'string')
    return Object.hasOwn(domErrors, cause.name) ? domErrors[cause.name] : StorageErrno.EIO;
  return StorageErrno.EIO;
}
