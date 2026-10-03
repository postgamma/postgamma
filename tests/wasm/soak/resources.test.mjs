/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
import assert from 'node:assert/strict';
import {resourceGate} from './resources.mjs';
const stable = Array.from({length: 20}, (_, i) => ({elapsedSeconds: 60 + i * 20, cycleEnd: true,
  rssBytes: 1000000, pssBytes: 500000, anonymousPssBytes: 400000, sharedMemoryPssBytes: 50000,
  filePssBytes: 50000, swapPssBytes: 0, retiredWorkerMaxAgeMs: 0,
  threads: 100, fileDescriptors: 400, virtualOpens: 260 + i % 3}));
const mixed = stable.flatMap(row => [{...row, cycleEnd: false, virtualOpens: 54, rssBytes: 500000}, row]);
assert.equal(resourceGate(mixed, 128 * 1024 ** 2).status, 'passed');
assert.equal(resourceGate(stable.slice(0, 9), 0).status, 'not-run');
assert.equal(resourceGate(stable.map(row => ({...row, virtualOpens: undefined})), 0).status, 'failed');
for (const field of ['rssBytes', 'anonymousPssBytes', 'sharedMemoryPssBytes', 'swapPssBytes', 'threads', 'fileDescriptors', 'virtualOpens']) {
  const leak = stable.map((row, i) => ({...row, [field]: row[field] + i * 1000}));
  assert.equal(resourceGate(leak, 100).status, 'failed', field);
}
const redistributed = stable.map((row, i) => ({...row, pssBytes: row.pssBytes + i * 1000, filePssBytes: row.filePssBytes + i * 1000}));
const redistribution = resourceGate(redistributed, 100);
assert.equal(redistribution.status, 'passed', 'Shared file PSS redistribution is not a new anonymous allocation');
assert(redistribution.totalPssGrowthBytes > 100 && redistribution.filePssGrowthBytes > 100);
for (const field of ['anonymousPssBytes', 'sharedMemoryPssBytes', 'filePssBytes'])
  assert.equal(resourceGate(stable.map(row => ({...row, [field]: undefined})), 100).status, 'failed', field);
assert.equal(resourceGate([{...stable[0], retiredWorkerMaxAgeMs: 10001}], 0).status, 'failed');
assert.equal(resourceGate([{...stable[0], retiredWorkerMaxAgeMs: 10000}], 0).status, 'not-run');
