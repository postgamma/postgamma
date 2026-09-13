/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
import {assert} from '../tests/helpers.js';
import {limits} from '../src/protocol/limits.js';

export function vectorBudget(modules: any[], parallel: boolean) {
  let parallelObservations = 0, workersLaunched = 0, parallelThreadsStarted = 0;
  for (const m of modules) {
    assert(m.samples >= 2 && m.peakThreads <= 13 && m.peakMemory <= 536870912, 'Vector workload stays within the existing target budget');
    assert(m.returnedWorkers === 13 && m.runningWorkers === 0 && m.droppedLogs === 0, 'Vector healthy Worker drain');
    assert(Object.values(m.resources).every(n => n === 0), 'Vector native resource release');
    assert(m.peakQueuedBytes <= limits.workerQueueBytes && m.peakPending <= limits.maxPendingCommands &&
      m.peakHostQueuedBytes <= limits.connectionQueueBytes * limits.maxConnections, 'Vector bridge queue bounds');
    let classes = 0, stack = 2 * 4194304;
    for (const line of m.output) {
      // Callback delivery can drop events on router contention. Count the
      // native stderr record, excluding its optional callback duplicate.
      if (/\[\d+\] DEBUG:\s+using 1 parallel workers$/.test(line)) workersLaunched++;
      if (line.includes('POSTGAMMA_RUNTIME '))
        parallelThreadsStarted += Number(/parallel_threads_started=(\d+)/.exec(line)?.[1] ?? NaN);
      if (!line.includes('POSTGAMMA_STACK ')) continue;
      const row = Object.fromEntries(line.split('POSTGAMMA_STACK ')[1].split(' ').map((s: string) => s.split('=')));
      for (const key of ['observation_failures', 'depth_validation_failures', 'accounting_failures', 'active_reservation_bytes'])
        assert(row[key] === '0', 'Vector stack accounting: ' + key);
      stack += Number(row.peak_reservation_bytes); classes++;
      if (row.class === 'parallel') parallelObservations += Number(row.observations);
    }
    assert(m.ioStackBytes === 0 || m.ioStackBytes === 2097152, 'Measured optional I/O stack');
    stack += m.ioStackBytes;
    assert(classes === 3 && stack <= 77594624, 'Vector stacks fit the existing role reservations');
  }
  if (parallel) assert(workersLaunched === 2 && parallelThreadsStarted === 2 && parallelObservations === 2, 'Both index builds actually launched and reclaimed a parallel worker');
  return {scope: 'Chromium, scalar pgvector', healthyModules: modules.length,
    peakLinearMemory: Math.max(...modules.map(m => m.peakMemory)), peakPthreads: Math.max(...modules.map(m => m.peakThreads)),
    workersLaunched, parallelThreadsStarted, parallelObservations, outsideLinearMemoryRssMeasured: false};
}
