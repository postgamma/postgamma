/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
import assert from 'node:assert/strict';
import {pathToFileURL} from 'node:url';
const {StorageErrno, storageErrorCode} = await import(pathToFileURL(process.argv[2]));
for (const [name, code] of [['QuotaExceededError', StorageErrno.ENOSPC], ['NotFoundError', StorageErrno.ENOENT],
  ['InvalidStateError', StorageErrno.EBUSY], ['NoModificationAllowedError', StorageErrno.EBUSY],
  ['NotAllowedError', StorageErrno.EACCES], ['TypeError', StorageErrno.EINVAL], ['NotSupportedError', StorageErrno.ENOTSUP]])
  assert.equal(storageErrorCode(new DOMException('injected', name)), code);
for (const cause of [null, undefined, 'error', 7, new Error('I/O failed'), {name: 'toString'}, {name: 'unknown'}])
  assert.equal(storageErrorCode(cause), StorageErrno.EIO, 'unknown failures cannot report success');
console.log('storage protocol: DOM failures and unknown exceptions preserve errno');
