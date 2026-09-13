/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
// The synchronous OPFS surface used on the dedicated owner Worker.
export interface AccessHandle {
  getSize(): number;
  read(data: Uint8Array, options: {at: number}): number;
  write(data: Uint8Array, options: {at: number}): number;
  truncate(size: number): void;
  flush(): void;
  close(): void;
}
export interface StorageFile {
  createSyncAccessHandle(): Promise<AccessHandle>;
}
export interface StorageDirectory {
  getFileHandle(name: string, options?: {create?: boolean}): Promise<StorageFile>;
  getDirectoryHandle(name: string, options?: {create?: boolean}): Promise<StorageDirectory>;
  entries(): AsyncIterableIterator<[string, unknown]>;
  removeEntry(name: string): Promise<void>;
}
