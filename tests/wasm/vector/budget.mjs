/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
import assert from 'node:assert/strict';
import path from 'node:path';
import {pathToFileURL} from 'node:url';
const {vectorBudget} = await import(pathToFileURL(path.resolve(process.argv[2])).href);
const {pthreadPoolSize} = await import(pathToFileURL(path.resolve(path.dirname(process.argv[2]), '../src/protocol/limits.js')).href);
const launch = '2026-09-13 00:00:00 GMT [1000000006] DEBUG:  using 1 parallel workers';
const callback = 'kernel: DEBUG: using 1 parallel workers';
const base = {
  samples: 2, peakThreads: pthreadPoolSize, peakMemory: 134217728, returnedWorkers: pthreadPoolSize,
  runningWorkers: 0, droppedLogs: 0, resources: {locks: 0, shmem: 0, dsm: 0, pipes: 0},
  ioStackBytes: 0,
  peakQueuedBytes: 0, peakPending: 0, peakHostQueuedBytes: 0,
};
const telemetry = ['POSTGAMMA_RUNTIME parallel_threads_started=2',
  ...['dedicated', 'client', 'parallel'].map(name =>
    `POSTGAMMA_STACK class=${name} observations=${name === 'parallel' ? 2 : 0} observation_failures=0 depth_validation_failures=0 accounting_failures=0 active_reservation_bytes=0 peak_reservation_bytes=0`)];
for (const count of [0, 1, 2]) {
  const output = [launch, launch, ...Array(count).fill(callback), ...telemetry];
  assert.equal(vectorBudget([{...base, output}], true).workersLaunched, 2);
}
assert.throws(() => vectorBudget([{...base, output: [launch, callback, callback, ...telemetry]}], true),
  /Both index builds actually launched/);
console.log('vector launch evidence: native records required; missing or duplicate callbacks do not alter counts');
